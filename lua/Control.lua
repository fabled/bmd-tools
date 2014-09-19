local cqueues = require 'cqueues'
local evdev = require 'evdev'

local function nop() end
local DummyCamera = {
	setTally = nop,
	setPower = nop,
	setTilt = nop,
	setPan = nop,
	setZoom = nop,
	gotoPreset = nop,
	run = nop,
}

local Control = { }

function Control:new(mixer)
	o = {
		mixer = nil,
		previewChannel = 0,
		previewCamera = DummyCamera,
		ptzSpeed = 10,
		cameras = {},
		cameraOrder = {},
		inputs = {},
		onKeyUp = {},
		onKeyDown = {},
		onKeyRepeat = {},
	}
	setmetatable(o, self)
	self.__index = self
	return o
end

function Control:setMixer(mixer)
	self.mixer = mixer
end

function Control:addCamera(ch, cam)
	self.cameras[ch] = cam or DummyCamera
	table.insert(self.cameraOrder, ch)
end

function Control:addInput(devname)
	self.inputs[devname] = true
end

function Control:getCamera(ch)
	return self.cameras[ch] or DummyCamera
end

function Control:bindKey(key, func)
	self.onKeyUp[key] = func
	self.onKeyDown[key] = func
	self.onKeyRepeat[key] = func
end

function Control:bindKeyUp(key, func)
	self.onKeyUp[key] = func
end

function Control:bindKeyDn(key, func)
	self.onKeyDown[key] = func
end


function Control:onPreviewChange(ch)
	self.previewChannel = ch
	self.previewCamera = self:getCamera(ch)
end

function Control:changePreview(diff)
	local ch = self.previewChannel
	local numCameras = #self.cameraOrder
	local new = 0
	for i, ich in ipairs(self.cameraOrder) do
		if ich == ch then new = 1 + (ich - 1 + diff + numCameras) % numCameras break end
	end
	self.mixer:setPreview(self.cameraOrder[new] or self.cameraOrder[1])
end

function Control:powerOnOff(val)
	if self.powerState == val then return end
	self.powerState = val
	for i, cam in pairs(self.cameras) do cam:setPower(val) end
end

function Control:pollInput(cq, devname)
	cq:wrap(function()
		local kps = {}
		print(("opening input device %s"):format(devname))
		local dev = evdev.Device(devname)
		--dev:grab()
		while true do
			cqueues.poll(dev)
			local timestamp, eventType, eventCode, value = dev:read()
			--print(devname, timestamp, eventType, eventCode, value)
			if eventType == evdev.EV_KEY then
				local f, dur
				kps[eventCode] = kps[eventCode] or timestamp
				dur = timestamp - kps[eventCode]
				if value == 0 then
					f = self.onKeyUp[eventCode] 
					kps[eventCode] = nil
				elseif value == 1 then
					f = self.onKeyDown[eventCode] 
				else
					f = self.onKeyRepeat[eventCode] 
				end
				if f then f(dur, value ~= 0) end
				--print(eventCode, dur, value)
			end
		end
	end)
end

function Control:run(cq)
	cq = cq or cqueues.new()
	self.mixer:run(cq, self)
	for input  in pairs(self.inputs) do self:pollInput(cq, input) end
	for _, cam in pairs(self.cameras) do cam:run(cq, self) end
	print(cq:loop())
end

return Control
