'use strict'

const {ipcRenderer} = require('electron')

const {guestInstanceId, openerId} = process

const usesNativeWindowOpen = process.argv.includes('--native-window-open')

require('./window-setup')(ipcRenderer, guestInstanceId, openerId, usesNativeWindowOpen)
