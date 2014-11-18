#!/usr/bin/lua5.2

package.path = package.path..";./?.lua"

local Control = require 'Control'
local AtemMixer = require 'AtemMixer'
local PanasonicAW = require 'PanasonicAW'
local Input = require 'Input'
local k = Input.keycodes

local lastpowerclick = 0
local c = Control:new()

local function powerOnOff(onoff)
	if onoff == false then c.mixer:setProgram(1000) end
	c:powerOnOff(onoff)
end

local function programOrGoto(e, cam, preset)
	if e.duration > 2.0 then
		cam:programPreset(preset)
	else
		cam:gotoPreset(preset)
	end
end

local function keyeventToSpeed(e)
	if e.value == Input.KEYVAL_UP then return 0 end
	if e.duration < 1.5 then return 10 end
	if e.duration < 4.0 then return 20 end
	return 30
end

local keymap = {
	onKey = {
		[k.KEY_LEFT]		= function(e) c.previewCamera:setPan(keyeventToSpeed(e)*-1) end,
		[k.KEY_RIGHT]		= function(e) c.previewCamera:setPan(keyeventToSpeed(e)) end,
		[k.KEY_UP]		= function(e) c.previewCamera:setTilt(keyeventToSpeed(e)) end,
		[k.KEY_DOWN]		= function(e) c.previewCamera:setTilt(keyeventToSpeed(e)*-1) end,
		[k.KEY_VOLUMEUP]	= function(e) c.previewCamera:setZoom(keyeventToSpeed(e)) end,
		[k.KEY_VOLUMEDOWN]	= function(e) c.previewCamera:setZoom(keyeventToSpeed(e)*-1) end,
	},
	onKeyClick = {
		[k.KEY_CHANNELUP]	= function() c:changePreview(1) end,
		[k.KEY_CHANNELDOWN]	= function() c:changePreview(-1) end,
		[k.KEY_1]		= function() c.cameras[5]:gotoPreset(1) end,
		[k.KEY_2]		= function() c.cameras[5]:gotoPreset(2) end,
		[k.KEY_3]		= function() c.cameras[5]:gotoPreset(3) end,
		[k.KEY_4]		= function() c.cameras[6]:gotoPreset(1) end,
		[k.KEY_5]		= function() c.cameras[6]:gotoPreset(2) end,
		[k.KEY_6]		= function() c.cameras[6]:gotoPreset(3) end,
		[k.KEY_7]		= function() c.cameras[6]:gotoPreset(4) end,
		[k.KEY_8]		= function() c.cameras[6]:gotoPreset(5) end,
		[k.KEY_9]		= function() c.cameras[6]:gotoPreset(6) end,
		[Input.MKEY_LSHIFT+k.KEY_8] = function(e) programOrGoto(e, c.cameras[5], 4) end,
		[k.KEY_0]		    = function(e) programOrGoto(e, c.cameras[5], 5) end,
		[Input.MKEY_LSHIFT+k.KEY_3] = function(e) programOrGoto(e, c.cameras[5], 6) end,
		[k.KEY_ENTER]		= function() c.mixer:doCut() end,
		[k.KEY_ESC]		= function() c.mixer:doFadeToBlack() end,
		[k.KEY_PROPS]		= function() c.mixer:autoDownstreamKey(0) end,
		[k.KEY_SLEEP]		= function() local now = os.time() powerOnOff(now-lastpowerclick >= 2.0) lastpowerclick=now end,
	}
}

c:addInput(Input:new("/dev/input/by-id/usb-G-Tech_Wireless_Dongle-event-kbd", keymap, true))
c:addInput(Input:new("/dev/input/by-id/usb-G-Tech_Wireless_Dongle-event-mouse", keymap, true))
c:setMixer(AtemMixer:new("192.168.0.10"))
c:addCamera(1)
c:addCamera(5, PanasonicAW:new("192.168.0.11"))
c:addCamera(6, PanasonicAW:new("192.168.0.12"))

c:run()
