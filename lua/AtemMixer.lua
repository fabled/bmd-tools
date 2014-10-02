local cqueues = require 'cqueues'
local socket = require 'cqueues.socket'
local struct = require 'struct'

--- ATEM Stuff
local AtemMixer = {
	CMD_ACK		= 0x8000,
	CMD_RESEND	= 0x2000,
	CMD_HELLO	= 0x1000,
	CMD_ACKREQ	= 0x0800,
	CMD_LENGTHMASK	= 0x07ff,
}

function AtemMixer:new(ip)
	o = {
		ip = ip,
		sock = socket.connect{host=ip, port=9910, type=socket.SOCK_DGRAM},
	}
	o.sock:setmode("bn", "bn")
	setmetatable(o, self)
	self.__index = self
	return o
end

function AtemMixer:send(flags, payload, ack_id)
	--print("sending to", self.ip)
	self.sock:write(struct.pack(">HHHHHH", bit32.bor(6*2 + #payload, flags), self.atem_uid, ack_id or 0, 0, 0, self.packet_id) .. payload)
	if bit32.band(flags, AtemMixer.CMD_ACK) == 0 then
		self.packet_id = self.packet_id + 1
	end
end

function AtemMixer:sendCmd(cmd, data)
	data = data or ""
	local pkt = struct.pack(">HHc4", 8+#data, 0, cmd)..data
	self:send(AtemMixer.CMD_ACKREQ, pkt)
end

function AtemMixer:doCut()
	self:sendCmd("DCut", struct.pack(">i4", 0))
end

function AtemMixer:autoDownstreamKey(id)
	self:sendCmd("DDsA", struct.pack(">i1i1i2", id, 0, 0))
end

function AtemMixer:setProgram(channel)
	self:sendCmd("CPgI", struct.pack(">i2i2", 0, channel))
end

function AtemMixer:setPreview(channel)
	self:sendCmd("CPvI", struct.pack(">i2i2", 0, channel))
end

function AtemMixer:onPrvI(data)
	local _, ndx = struct.unpack(">HH", data)
	self.control:onPreviewChange(ndx)
end

function AtemMixer:onTlIn(data)
	local num = data:byte(2)
	for i = 1, num do
		self.control:getCamera(i):setTally(bit32.band(data:byte(i+2), 1) == 1) 
	end
end

function AtemMixer:recv()
	local pkt, err = self.sock:read("*a")
	if err then return false end
	if #pkt < 6 then return true end

	--print(("Got %d bytes from ATEM"):format(#pkt))
	local flags, uid, ack_id, _, _, packet_id = struct.unpack(">HHHHHH", pkt)
	local length = bit32.band(flags, AtemMixer.CMD_LENGTHMASK)

	self.atem_uid = uid
	if bit32.band(flags, bit32.bor(AtemMixer.CMD_ACKREQ, AtemMixer.CMD_HELLO)) ~= 0 then
		self:send(AtemMixer.CMD_ACK, "", packet_id)
	end
	if bit32.band(flags, AtemMixer.CMD_HELLO) ~= 0 then return true end

	-- handle payload
	local off = 12
	while off + 8 < #pkt do
		local len, _, type = struct.unpack(">HHc4", pkt, off+1)
		--print(("  --> %s len %d"):format(type, len))
		local handler = "on"..type
		if self[handler] then self[handler](self, pkt:sub(off+1+8,off+len)) end
		off = off + len
	end

	return true
end

function AtemMixer:send_hello()
	self.sock:settimeout(5)
	self.atem_uid = 0x1337
	self.packet_id = 0
	self:send(AtemMixer.CMD_HELLO, string.char(0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00))
end

function AtemMixer:run(cq, control)
	self.control = control
	cq:wrap(function()
		while true do
			self:send_hello()
			local packet_id = 1
			while self:recv() do end
			cqueues.sleep(5.0)
		end
	end)
end

return AtemMixer
