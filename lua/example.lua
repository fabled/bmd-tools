#!/usr/bin/lua5.2

package.path = package.path..";./?.lua"

local k = require 'evdev.constants'
local Control = require 'Control'
local AtemMixer = require 'AtemMixer'
local PanasonicAW = require 'PanasonicAW'
local lastpowerclick = 0

local c = Control:new()

local function powerOnOff(onoff)
	if onoff == false then c.mixer:setProgram(1000) end
	c:powerOnOff(onoff)
end

local function durToSpeed(dur, st)
	if not st then return 0 end
	if dur < 1.5 then return 10 end
	if dur < 4.0 then return 20 end
	return 30
end

c:addInput("/dev/input/by-id/usb-G-Tech_Wireless_Dongle-event-kbd")
c:addInput("/dev/input/by-id/usb-G-Tech_Wireless_Dongle-event-mouse")
c:setMixer(AtemMixer:new("192.168.0.10"))
c:addCamera(1)
c:addCamera(5, PanasonicAW:new("192.168.0.11"))
c:addCamera(6, PanasonicAW:new("192.168.0.12"))

c:bindKey(k.KEY_LEFT,       function(dur,st) c.previewCamera:setPan(durToSpeed(dur,st)*-1) end)
c:bindKey(k.KEY_RIGHT,      function(dur,st) c.previewCamera:setPan(durToSpeed(dur,st)) end)
c:bindKey(k.KEY_UP,         function(dur,st) c.previewCamera:setTilt(durToSpeed(dur,st)) end)
c:bindKey(k.KEY_DOWN,       function(dur,st) c.previewCamera:setTilt(durToSpeed(dur,st)*-1) end)
c:bindKey(k.KEY_VOLUMEUP,   function(dur,st) c.previewCamera:setZoom(durToSpeed(dur,st)) end)
c:bindKey(k.KEY_VOLUMEDOWN, function(dur,st) c.previewCamera:setZoom(durToSpeed(dur,st)*-1) end)
c:bindKeyDn(k.KEY_CHANNELUP,   function() c:changePreview(1) end)
c:bindKeyDn(k.KEY_CHANNELDOWN, function() c:changePreview(-1) end)
c:bindKeyDn(k.KEY_1, function() c.cameras[5]:gotoPreset(1) end)
c:bindKeyDn(k.KEY_2, function() c.cameras[5]:gotoPreset(2) end)
c:bindKeyDn(k.KEY_3, function() c.cameras[5]:gotoPreset(3) end)
c:bindKeyDn(k.KEY_4, function() c.cameras[6]:gotoPreset(1) end)
c:bindKeyDn(k.KEY_5, function() c.cameras[6]:gotoPreset(2) end)
c:bindKeyDn(k.KEY_6, function() c.cameras[6]:gotoPreset(3) end)
c:bindKeyDn(k.KEY_7, function() c.cameras[6]:gotoPreset(4) end)
c:bindKeyDn(k.KEY_8, function() c.cameras[6]:gotoPreset(5) end)
c:bindKeyDn(k.KEY_9, function() c.cameras[6]:gotoPreset(6) end)
c:bindKeyDn(k.KEY_ENTER, function() c.mixer:doCut() end)
c:bindKeyDn(k.KEY_PROPS, function() c.mixer:autoDownstreamKey(0) end)
c:bindKeyDn(k.KEY_SLEEP, function() local now = os.time() powerOnOff(now-lastpowerclick >= 2.0) lastpowerclick=now end)

c:run()
