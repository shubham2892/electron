#include "inspector_agent.h"

#include "inspector_io.h"
#include "env.h"
#include "env-inl.h"
#include "node.h"
#include "v8-inspector.h"
#include "v8-platform.h"
#include "util.h"
#include "zlib.h"

#include "libplatform/libplatform.h"

#include <string.h>
#include <vector>

namespace node {
namespace inspector {
namespace {
using v8::Context;
using v8::Function;
using v8::FunctionCallbackInfo;
using v8::HandleScope;
using v8::Isolate;
using v8::Local;
using v8::Object;
using v8::String;
using v8::Value;

using v8_inspector::StringBuffer;
using v8_inspector::StringView;
using v8_inspector::V8Inspector;

std::unique_ptr<StringBuffer> ToProtocolString(Isolate* isolate,
                                               Local<Value> value) {
  TwoByteValue buffer(isolate, value);
  return StringBuffer::create(StringView(*buffer, buffer.length()));
}

// void InspectorConsoleCall(Agent *agent, const v8::FunctionCallbackInfo<Value>& info) {
//   Isolate* isolate = info.GetIsolate();
//   HandleScope handle_scope(isolate);
//   Local<Context> context = isolate->GetCurrentContext();
//   CHECK_LT(2, info.Length());
//   std::vector<Local<Value>> call_args;
//   for (int i = 3; i < info.Length(); ++i) {
//     call_args.push_back(info[i]);
//   }
//   Environment* env = Environment::GetCurrent(isolate);
//   if (agent->enabled()) {
//     Local<Value> inspector_method = info[0];
//     CHECK(inspector_method->IsFunction());
//     Local<Value> config_value = info[2];
//     CHECK(config_value->IsObject());
//     Local<Object> config_object = config_value.As<Object>();
//     Local<String> in_call_key = FIXED_ONE_BYTE_STRING(isolate, "in_call");
//     if (!config_object->Has(context, in_call_key).FromMaybe(false)) {
//       CHECK(config_object->Set(context,
//                                in_call_key,
//                                v8::True(isolate)).FromJust());
//       CHECK(!inspector_method.As<Function>()->Call(context,
//                                                    info.Holder(),
//                                                    call_args.size(),
//                                                    call_args.data()).IsEmpty());
//     }
//     CHECK(config_object->Delete(context, in_call_key).FromJust());
//   }
//
//   Local<Value> node_method = info[1];
//   CHECK(node_method->IsFunction());
//   static_cast<void>(node_method.As<Function>()->Call(context,
//                                                      info.Holder(),
//                                                      call_args.size(),
//                                                      call_args.data()));
// }

// void CallAndPauseOnStart(
//     const v8::FunctionCallbackInfo<v8::Value>& args) {
//   Environment* env = Environment::GetCurrent(args);
//   CHECK_GT(args.Length(), 1);
//   CHECK(args[0]->IsFunction());
//   std::vector<v8::Local<v8::Value>> call_args;
//   for (int i = 2; i < args.Length(); i++) {
//     call_args.push_back(args[i]);
//   }
//
//   env->inspector_agent()->PauseOnNextJavascriptStatement("Break on start");
//   v8::MaybeLocal<v8::Value> retval =
//       args[0].As<v8::Function>()->Call(env->context(), args[1],
//                                        call_args.size(), call_args.data());
//   args.GetReturnValue().Set(retval.ToLocalChecked());
// }
}  // namespace

// Used in NodeInspectorClient::currentTimeMS() below.
const int NANOS_PER_MSEC = 1000000;
const int CONTEXT_GROUP_ID = 1;

class ChannelImpl final : public v8_inspector::V8Inspector::Channel {
 public:
  explicit ChannelImpl(V8Inspector* inspector,
                       InspectorSessionDelegate* delegate)
                       : delegate_(delegate) {
    session_ = inspector->connect(1, this, StringView());
  }

  virtual ~ChannelImpl() {}

  void dispatchProtocolMessage(const StringView& message) {
    session_->dispatchProtocolMessage(message);
  }

  bool waitForFrontendMessage() {
    return delegate_->WaitForFrontendMessage();
  }

  void schedulePauseOnNextStatement(const std::string& reason) {
    std::unique_ptr<StringBuffer> buffer = Utf8ToStringView(reason);
    session_->schedulePauseOnNextStatement(buffer->string(), buffer->string());
  }

  InspectorSessionDelegate* delegate() {
    return delegate_;
  }

 private:
  void sendResponse(
      int callId,
      std::unique_ptr<v8_inspector::StringBuffer> message) override {
    sendMessageToFrontend(message->string());
  }

  void sendNotification(
      std::unique_ptr<v8_inspector::StringBuffer> message) override {
    sendMessageToFrontend(message->string());
  }

  void flushProtocolNotifications() override { }

  void sendMessageToFrontend(const StringView& message) {
    delegate_->OnMessage(message);
  }

  InspectorSessionDelegate* const delegate_;
  std::unique_ptr<v8_inspector::V8InspectorSession> session_;
};

class NodeInspectorClient : public v8_inspector::V8InspectorClient {
 public:
  NodeInspectorClient(node::Environment* env,
                      v8::Platform* platform) : env_(env),
                                                platform_(platform),
                                                terminated_(false),
                                                running_nested_loop_(false) {
    inspector_ = V8Inspector::create(env->isolate(), this);
  }

  void runMessageLoopOnPause(int context_group_id) override {
    CHECK_NE(channel_, nullptr);
    if (running_nested_loop_)
      return;
    terminated_ = false;
    running_nested_loop_ = true;
    while (!terminated_ && channel_->waitForFrontendMessage()) {
      while (v8::platform::PumpMessageLoop(platform_, env_->isolate()))
        {}
    }
    terminated_ = false;
    running_nested_loop_ = false;
  }

  double currentTimeMS() override {
    return uv_hrtime() * 1.0 / NANOS_PER_MSEC;
  }

  void contextCreated(Local<Context> context, const std::string& name) {
    std::unique_ptr<StringBuffer> name_buffer = Utf8ToStringView(name);
    v8_inspector::V8ContextInfo info(context, CONTEXT_GROUP_ID,
                                     name_buffer->string());
    inspector_->contextCreated(info);
  }

  void contextDestroyed(Local<Context> context) {
    inspector_->contextDestroyed(context);
  }

  void quitMessageLoopOnPause() override {
    terminated_ = true;
  }

  void connectFrontend(InspectorSessionDelegate* delegate) {
    CHECK_EQ(channel_, nullptr);
    channel_ = std::unique_ptr<ChannelImpl>(
        new ChannelImpl(inspector_.get(), delegate));
  }

  void disconnectFrontend() {
    quitMessageLoopOnPause();
    channel_.reset();
  }

  void dispatchMessageFromFrontend(const StringView& message) {
    CHECK_NE(channel_, nullptr);
    channel_->dispatchProtocolMessage(message);
  }

  Local<Context> ensureDefaultContextInGroup(int contextGroupId) override {
    return env_->context();
  }

  void FatalException(Local<Value> error, Local<v8::Message> message) {
    Local<Context> context = env_->context();

    int script_id = message->GetScriptOrigin().ScriptID()->Value();

    Local<v8::StackTrace> stack_trace = message->GetStackTrace();

    if (!stack_trace.IsEmpty() &&
        stack_trace->GetFrameCount() > 0 &&
        script_id == stack_trace->GetFrame(0)->GetScriptId()) {
      script_id = 0;
    }

    const uint8_t DETAILS[] = "Uncaught";

    Isolate* isolate = context->GetIsolate();

    inspector_->exceptionThrown(
        context,
        StringView(DETAILS, sizeof(DETAILS) - 1),
        error,
        ToProtocolString(isolate, message->Get())->string(),
        ToProtocolString(isolate, message->GetScriptResourceName())->string(),
        message->GetLineNumber(context).FromMaybe(0),
        message->GetStartColumn(context).FromMaybe(0),
        inspector_->createStackTrace(stack_trace),
        script_id);
  }

  ChannelImpl* channel() {
    return channel_.get();
  }

 private:
  node::Environment* env_;
  v8::Platform* platform_;
  bool terminated_;
  bool running_nested_loop_;
  std::unique_ptr<V8Inspector> inspector_;
  std::unique_ptr<ChannelImpl> channel_;
};

Agent::Agent(Environment* env) : parent_env_(env),
                                 inspector_(nullptr),
                                 platform_(nullptr),
                                 enabled_(false) {}

// Destructor needs to be defined here in implementation file as the header
// does not have full definition of some classes.
Agent::~Agent() {
}

bool Agent::Start(v8::Platform* platform, const char* path,
                  const DebugOptions& options) {
  path_ = path == nullptr ? "" : path;
  debug_options_ = options;
  inspector_ =
      std::unique_ptr<NodeInspectorClient>(
          new NodeInspectorClient(parent_env_, platform));
  inspector_->contextCreated(parent_env_->context(), "Main Process Context");
  platform_ = platform;
  return StartIoThread();
}

bool Agent::StartIoThread() {
  if (io_ != nullptr)
    return true;

  CHECK_NE(inspector_, nullptr);

  enabled_ = true;
  io_ = std::unique_ptr<InspectorIo>(
      new InspectorIo(this, parent_env_, platform_, path_, debug_options_));
  if (!io_->Start()) {
    inspector_.reset();
    return false;
  }

  v8::Isolate* isolate = parent_env_->isolate();

  // Send message to enable debug in workers
  HandleScope handle_scope(isolate);
  Local<Object> process_object = parent_env_->process_object();
  Local<Value> emit_fn =
      process_object->Get(FIXED_ONE_BYTE_STRING(isolate, "emit"));
  // In case the thread started early during the startup
  if (!emit_fn->IsFunction())
    return true;

  Local<Object> message = Object::New(isolate);
  message->Set(FIXED_ONE_BYTE_STRING(isolate, "cmd"),
               FIXED_ONE_BYTE_STRING(isolate, "NODE_DEBUG_ENABLED"));
  Local<Value> argv[] = {
    FIXED_ONE_BYTE_STRING(isolate, "internalMessage"),
    message
  };
  MakeCallback(parent_env_, process_object.As<Value>(), emit_fn.As<Function>(),
               arraysize(argv), argv);

  return true;
}

void Agent::Stop() {
  if (io_ != nullptr)
    io_->Stop();
}

void Agent::Connect(InspectorSessionDelegate* delegate) {
  enabled_ = true;
  inspector_->connectFrontend(delegate);
}

bool Agent::IsConnected() {
  return io_ && io_->IsConnected();
}

bool Agent::IsStarted() {
  return !!inspector_;
}

void Agent::WaitForDisconnect() {
  CHECK_NE(inspector_, nullptr);
  inspector_->contextDestroyed(parent_env_->context());
  if (io_ != nullptr) {
    io_->WaitForDisconnect();
  }
}

void Agent::FatalException(Local<Value> error, Local<v8::Message> message) {
  if (!IsStarted())
    return;
  inspector_->FatalException(error, message);
  WaitForDisconnect();
}

void Agent::Dispatch(const StringView& message) {
  CHECK_NE(inspector_, nullptr);
  inspector_->dispatchMessageFromFrontend(message);
}

void Agent::Disconnect() {
  CHECK_NE(inspector_, nullptr);
  inspector_->disconnectFrontend();
}

void Agent::RunMessageLoop() {
  CHECK_NE(inspector_, nullptr);
  inspector_->runMessageLoopOnPause(CONTEXT_GROUP_ID);
}

void Agent::PauseOnNextJavascriptStatement(const std::string& reason) {
  ChannelImpl* channel = inspector_->channel();
  if (channel != nullptr)
    channel->schedulePauseOnNextStatement(reason);
}

// static
void Agent::InitJSBindings(Local<Object> target, Local<Value> unused,
                           Local<Context> context, void* priv) {
  // Environment* env = Environment::GetCurrent(context);
  // Agent* agent = env->inspector_agent();
  // env->SetMethod(target, "consoleCall", base::Bind(InspectorConsoleCall, agent);
  // if (agent->debug_options_.wait_for_connect())
  //   env->SetMethod(target, "callAndPauseOnStart", CallAndPauseOnStart);
}
}  // namespace inspector
}  // namespace node

NODE_MODULE_CONTEXT_AWARE_BUILTIN(inspector,
                                  node::inspector::Agent::InitJSBindings);
