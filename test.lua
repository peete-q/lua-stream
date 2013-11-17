
stream = require 'stream'

function test(cond)
	if cond then
		print'[ok]'
	else
		print'[failed]'
	end
end

local s = stream.new()
s:writef('bBw', 100, -1, 1000)
local p1, p2 = s:write(false, true, nil, 0)
s:insert(p1, 'ss')
local tb = {
	[1] = 1,
	s = 's',
}
tb.ref = tb
s:write(tb)
local p1, p2 = s:writef('oozz', nil, false, 's2', 's3')
s:insertf(p1, 'Dss', 9999, 's0', 's1')

local a, b, c = s:readf('bBw')
test(a == 100 and b == 255 and c == 1000)
local a, b, c, d, e = s:read(5)
test(a == 'ss' and b == false and c == true and d == nil and e == 0)
local tb2 = s:read()
test(tb[1] == tb2[1] and tb.s == tb2.s and tb2.ref == tb2)
local n = s:readf("D")
test(n == 9999)
local a, b, c, d, e, f = s:readf('s2s2oozz')
test(a == 's0' and b == 's1' and c == nil and d == false and e == 's2' and f == 's3')

local s2 = stream.new()
print('------')
s:seek(0)
local p1, p2 = s2:extract(s)
s2:insertf(0, "D", 1)
local a = s2:readf("D")
test(a == 1)
local a, b, c = s2:readf('bBw')
test(a == 100 and b == 255 and c == 1000)
local a, b, c, d, e = s2:read(5)
test(a == 'ss' and b == false and c == true and d == nil and e == 0)
local tb2 = s2:read()
test(tb[1] == tb2[1] and tb.s2 == tb2.s2 and tb2.ref == tb2)
local n = s2:readf("D")
test(n == 9999)
local a, b, c, d, e, f = s2:readf('s2s2oozz')
test(a == 's0' and b == 's1' and c == nil and d == false and e == 's2' and f == 's3')
