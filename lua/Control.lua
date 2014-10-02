local cqueues = require 'cqueues'

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

function Control:addInput(dev)
	table.insert(self.inputs, dev)
end

function Control:getCamera(ch)
	return self.cameras[ch] or DummyCamera
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

function Control:run(cq)
	cq = cq or cqueues.new()
	self.mixer:run(cq, self)
	for _, input in pairs(self.inputs)  do input:run(cq) end
	for _, cam   in pairs(self.cameras) do cam:run(cq, self) end
	print(cq:loop())
end

return Control
