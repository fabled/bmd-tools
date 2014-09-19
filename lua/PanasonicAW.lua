local cqueues = require 'cqueues'
local socket = require 'cqueues.socket'
local condition = require 'cqueues.condition'

local PanasonicAW = {}

local function http_get(ip, uri)
	local s = socket.connect(ip, 80)
	s:write(("GET /%s HTTP/1.1\nHost: %s\nConnection: Close\n\n"):format(uri, ip))
	local status = s:read("*l")
	-- FIXME: Could read the body, but cqueues seems to hang
	s:close()
	--print(status)
	return status
end

function PanasonicAW:new(ip)
	o = {
		ip = ip,
		cmdqueue = {},
		cmdstate = {},
		cmdcond = condition.new(),
	}
	setmetatable(o, self)
	self.__index = self
	return o
end

function PanasonicAW:awptz(cmd, fmt, arg, always)
	local val = fmt:format(arg)
	if not always then
		if self.cmdstate[cmd] == val then return end
		self.cmdstate[cmd] = val
	end
	self.cmdqueue[cmd] = val
	self.cmdcond:signal(1)
end

function PanasonicAW:setTally(onoff) self:awptz("DA", "%d", onoff and 1 or 0) end
function PanasonicAW:setPower(onoff) self:awptz("O", "%d", onoff and 1 or 0, true) end
function PanasonicAW:setTilt(speed)  self:awptz("T", "%02d", 50+speed) end
function PanasonicAW:setPan(speed)   self:awptz("P", "%02d", 50+speed) end
function PanasonicAW:setZoom(speed)  self:awptz("Z", "%02d", 50+speed) end
function PanasonicAW:gotoPreset(no)  self:awptz("R", "%02d", no-1, true) end

function PanasonicAW:run(cq, control)
	cq:wrap(function()
		while true do
			local q
			q, self.cmdqueue = self.cmdqueue, {}
			for cmd, val in pairs(q) do
				local uri = ("/cgi-bin/aw_ptz?cmd=%%23%s%s&res=1"):format(cmd, val)
				local status = http_get(self.ip, uri)
				print("Posting", self.ip, uri, status)
			end
			if #self.cmdqueue == 0 then
				self.cmdcond:wait()
			end
		end
	end)
end

return PanasonicAW
