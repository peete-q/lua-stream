#include <assert.h>
#include <ctype.h>
#include <stddef.h>
#include <string.h>
#include <malloc.h>
#include <stdlib.h>
#include <limits.h>

#include "buffer.h"
#include "stream.h"

#ifndef MIN
#define MIN(x, y) ((x) < (y) ? x : y)
#endif
#ifndef MAX
#define MAX(x, y) ((x) > (y) ? x : y)
#endif

#if (LUA_VERSION_NUM >= 502)
#define luaL_register(L,n,f)	luaL_newlib(L,f)
#endif

#define luaL_check(c, ...)		if (!(c)) luaL_error(L, __VA_ARGS__)

#define AUTHORS 	"Peter.Q"
#define VERSION		"LuaStream 1.0"
#define RELEASE		"LuaStream 1.0.1"

#define REFS_SIZE	256
#define BUFF_SIZE	256
#define LIB_NAME	"stream*"
#define DEF_ENDIAN	1

#define F_SIGNED_BYTE		'b'
#define F_UNSIGNED_BYTE		'B'
#define F_SIGNED_WORD		'w'
#define F_UNSIGNED_WORD		'W'
#define F_SIGNED_DWORD		'd'
#define F_UNSIGNED_DWORD	'D'
#define F_FLOAT				'f'
#define F_ZSTRING			'z'
#define F_STRING			's'
#define F_OBJECT			'o'

struct lua_Stream {
	int ref;
	size_t pos;
	buffer_t buf;
};

static union {
	int dummy;
	char endian;
} const native = {1};

enum {
	OP_NIL,
	OP_TRUE,
	OP_FALSE,
	OP_ZERO,
	OP_FLOAT,
	OP_INT,
	OP_STRING,
	OP_TABLE,
	OP_TABLE_REF,
	OP_TABLE_DELIMITER,
	OP_TABLE_END,
};

struct writer_t {
	struct {
		const void* ptr;
		size_t pos;
	} refs[REFS_SIZE];
	
	size_t pos;
	size_t count;
};

struct reader_t {
	struct {
		size_t idx;
		size_t pos;
	} refs[REFS_SIZE];
	
	size_t pos;
	size_t count;
};

static void correctbytes (void *data, int size)
{
	if (native.endian != DEF_ENDIAN)
	{
		char *ptr = (char*) data;
		int i = 0;
		while (i < --size)
		{
			char temp = ptr[i];
			ptr[i++] = ptr[size];
			ptr[size] = temp;
		}
	}
}

static size_t buffer_writeint(buffer_t *buf, lua_Integer n)
{
	char *a = (char*)&n;
	size_t i;
	correctbytes(&n, sizeof(n));
	for (i = sizeof(n); i > 0 && a[i - 1] == 0; --i);
	buffer_write(buf, a, i);
	return i;
}

static size_t buffer_writefloat(buffer_t *buf, lua_Number n)
{
	float f = n;
	if (n == (lua_Number)f)
	{
		correctbytes(&f, sizeof(f));
		buffer_write(buf, &f, sizeof(f));
		return sizeof(f);
	}
	correctbytes(&n, sizeof(n));
	buffer_write(buf, &n, sizeof(n));
	return sizeof(n);
}

static int buffer_writeobject(lua_State *L, buffer_t *buf, int idx, struct writer_t *W)
{
	int top = lua_gettop(L);
	int type = lua_type(L, idx);
	switch(type)
	{
		case LUA_TNIL:
			buffer_writebyte(buf, OP_NIL);
			break;
		case LUA_TBOOLEAN:
			buffer_writebyte(buf, lua_toboolean(L, idx) ? OP_TRUE : OP_FALSE);
			break;
		case LUA_TNUMBER:
		{
			lua_Number n = lua_tonumber(L, idx);
			if (n == 0)
				buffer_writebyte(buf, OP_ZERO);
			else if (floor(n) == n)
			{
				size_t size, pos = buffer_tell(buf);
				buffer_writebyte(buf, OP_INT);
				size = buffer_writeint(buf, n);
				*buffer_at(buf, pos) |= size << 4;
			}
			else
			{
				size_t size, pos = buffer_tell(buf);
				buffer_writebyte(buf, OP_FLOAT);
				size = buffer_writefloat(buf, n);
				*buffer_at(buf, pos) |= size << 4;
			}
			break;
		}
		case LUA_TSTRING:
		{
			size_t len;
			const char* str = lua_tolstring(L, idx, &len);
			size_t size, pos = buffer_tell(buf);
			buffer_writebyte(buf, OP_STRING);
			size = buffer_writeint(buf, len);
			*buffer_at(buf, pos) |= size << 4;
			buffer_write(buf, str, len);
			break;
		}
		case LUA_TTABLE:
		{
			const void *ptr = lua_topointer(L, idx);
			size_t i;
			for (i = 0; i < W->count; ++i)
			{
				if (W->refs[i].ptr == ptr)
				{
					buffer_writebyte(buf, OP_TABLE_REF);
					buffer_writebyte(buf, W->refs[i].pos);
					goto end;
				}
			}
			luaL_check(W->count < REFS_SIZE, "table refs overflow %d", REFS_SIZE);
			W->refs[W->count].ptr = ptr;
			W->refs[W->count++].pos = buffer_tell(buf) - W->pos;
			
			buffer_writebyte(buf, OP_TABLE);
			lua_pushnil(L);
			i = 1;
			while (lua_next(L, idx))
			{
				if (lua_isnumber(L, -2) && lua_tonumber(L, -2) == i++)
				{
					buffer_writeobject(L, buf, lua_gettop(L), W);
					lua_pop(L, 1);
				}
				else break;
			}
			buffer_writebyte(buf, OP_TABLE_DELIMITER);
			if (lua_gettop(L) > top)
			{
				do
				{
					buffer_writeobject(L, buf, lua_gettop(L) - 1, W);
					buffer_writeobject(L, buf, lua_gettop(L), W);
					lua_pop(L, 1);
				}
				while (lua_next(L, idx));
			}
			buffer_writebyte(buf, OP_TABLE_END);
			break;
		}
		default:
			lua_settop(L, top);
			luaL_error(L, "unexpected type:%s", lua_typename(L, type));
			return 0;
	}
end:
	lua_settop(L, top);
	return 1;
}

static int buffer_readobject(lua_State *L, const char *data, size_t pos, size_t size, struct reader_t *R)
{
	int op = data[pos++];
	switch(op & 0x0f)
	{
		case OP_NIL:
			lua_pushnil(L);
			break;
		case OP_TRUE:
			lua_pushboolean(L, 1);
			break;
		case OP_FALSE:
			lua_pushboolean(L, 0);
			break;
		case OP_ZERO:
			lua_pushnumber(L, 0);
			break;
		case OP_INT:
		{
			int len = (op & 0xf0) >> 4;
			lua_Integer n = 0;
			memcpy(&n, data + pos, len);
			correctbytes(&n, len);
			lua_pushnumber(L, n);
			pos += len;
			break;
		}
		case OP_FLOAT:
		{
			int len = (op & 0xf0) >> 4;
			if (len == sizeof(float))
			{
				float n = 0;
				memcpy(&n, data + pos, len);
				correctbytes(&n, len);
				lua_pushnumber(L, n);
			}
			else
			{
				double n = 0;
				memcpy(&n, data + pos, len);
				correctbytes(&n, len);
				lua_pushnumber(L, n);
			}
			pos += len;
			break;
		}
		case OP_STRING:
		{
			int len = (op & 0xf0) >> 4;
			size_t n = 0;
			memcpy(&n, data + pos, len);
			correctbytes(&n, len);
			pos += len;
			lua_pushlstring(L, data + pos, n);
			pos += n;
			break;
		}
		case OP_TABLE:
		{
			size_t i;
			lua_newtable(L);
			lua_pushvalue(L, -1);
			luaL_check(R->count < REFS_SIZE, "table refs overflow %d", REFS_SIZE);
			R->refs[R->count].pos = pos - R->pos - 1;
			R->refs[R->count++].idx = luaL_ref(L, LUA_REGISTRYINDEX);
			for (i = 1; data[pos] != OP_TABLE_DELIMITER; ++i)
			{
				luaL_check(pos < size, "bad data, when read index %d:%d", pos, size);
				pos = buffer_readobject(L, data, pos, size, R);
				lua_rawseti(L, -2, i);
			}
			pos++;
			while (data[pos] != OP_TABLE_END)
			{
				luaL_check(pos < size, "bad data, when read key %d:%d", pos, size);
				pos = buffer_readobject(L, data, pos, size, R);
				
				luaL_check(pos < size, "bad data, when read value %d:%d", pos, size);
				pos = buffer_readobject(L, data, pos, size, R);
				lua_settable(L, -3);
			}
			pos++;
			break;
		}
		case OP_TABLE_REF:
		{
			size_t i, where = data[pos++];
			for (i = 0; i < R->count; ++i)
			{
				if (R->refs[i].pos == where)
				{
					lua_rawgeti(L, LUA_REGISTRYINDEX, R->refs[i].idx);
					return pos;
				}
			}
			luaL_error(L, "bad ref: %d", where);
			return 0;
		}
		default:
			luaL_error(L, "bad opecode: %d", op);
			return 0;
	}
	return pos;
}

static int lib_new (lua_State *L)
{
	lua_Stream *self = (lua_Stream *)lua_newuserdata(L, sizeof(lua_Stream));
	self->buf = buffer_new(BUFF_SIZE);
	self->pos = 0;
	self->ref = LUA_REFNIL;
	luaL_getmetatable(L, LIB_NAME);
	lua_setmetatable(L, -2);
	return 1;
}

static int lib_clone (lua_State *L)
{
	lua_Stream *self = (lua_Stream *)luaL_checkudata(L, 1, LIB_NAME);
	lua_Stream *other = (lua_Stream *)lua_newuserdata(L, sizeof(lua_Stream));
	luaL_check(self->buf, "%s (released) #1", LIB_NAME);
	other->buf = buffer_new(buffer_tell(&self->buf));
	other->pos = self->pos;
	other->ref = LUA_REFNIL;
	buffer_write(&other->buf, buffer_ptr(&self->buf), buffer_tell(&self->buf));
	luaL_getmetatable(L, LIB_NAME);
	lua_setmetatable(L, -2);
	return 1;
}

static int lib_writefrom (lua_State *L)
{
	lua_Stream *self = (lua_Stream *)luaL_checkudata(L, 1, LIB_NAME);
	lua_Stream *other = (lua_Stream *)luaL_checkudata(L, 2, LIB_NAME);
	size_t pos, size;
	luaL_check(self->buf, "%s (released) #1", LIB_NAME);
	luaL_check(other->buf, "%s (released) #2", LIB_NAME);
	size = luaL_checkint(L, 3);
	luaL_check(other->pos + size <= buffer_tell(&other->buf), "size overflow #3");
	pos = buffer_tell(&self->buf);
	buffer_write(&self->buf, buffer_at(&other->buf, other->pos), size);
	other->pos += size;
	lua_pushnumber(L, pos);
	lua_pushnumber(L, buffer_tell(&self->buf));
	return 2;
}

static int lib_insertfrom (lua_State *L)
{
	lua_Stream *self = (lua_Stream *)luaL_checkudata(L, 1, LIB_NAME);
	lua_Stream *other = (lua_Stream *)luaL_checkudata(L, 3, LIB_NAME);
	size_t where, size, pos = luaL_checkint(L, 2);
	luaL_check(self->buf, "%s (released) #1", LIB_NAME);
	luaL_check(pos <= buffer_tell(&self->buf), "out of range #2");
	luaL_check(other->buf, "%s (released) #3", LIB_NAME);
	size = luaL_checkint(L, 4);
	luaL_check(other->pos + size <= buffer_tell(&other->buf), "size overflow #4");
	buffer_insert(&self->buf, pos, buffer_at(&other->buf, other->pos), size);
	other->pos += size;
	lua_pushnumber(L, pos);
	lua_pushnumber(L, buffer_tell(&self->buf));
	return 2;
}

static int lib_release (lua_State *L)
{
	lua_Stream *self = (lua_Stream *)luaL_checkudata(L, 1, LIB_NAME);
	if (self->buf)
		buffer_delete(&self->buf);
	return 0;
}

static int lib_mt_tostring (lua_State *L)
{
	lua_Stream *self = (lua_Stream *)luaL_checkudata(L, 1, LIB_NAME);
	if (self->buf)
		lua_pushfstring(L, "%s (%p)", LIB_NAME, self);
	else
		lua_pushfstring(L, "%s (released)", LIB_NAME);
	return 1;
}

static int lib_tostring (lua_State *L)
{
	lua_Stream *self = (lua_Stream *)luaL_checkudata(L, 1, LIB_NAME);
	if (self->buf)
		lua_pushlstring(L, buffer_ptr(&self->buf), buffer_tell(&self->buf));
	else
		lua_pushnil(L);
	return 1;
}

static int lib_write (lua_State *L)
{
	size_t pos;
	int i, top = lua_gettop(L);
	struct writer_t W;
	lua_Stream *self = (lua_Stream *)luaL_checkudata(L, 1, LIB_NAME);
	luaL_check(self->buf, "%s (released) #1", LIB_NAME);
	pos = buffer_tell(&self->buf);
	W.count = 0;
	W.pos = pos;
	for (i = 2; i <= top; ++i, W.pos = buffer_tell(&self->buf))
		buffer_writeobject(L, &self->buf, i, &W);
	lua_pushnumber(L, pos);
	lua_pushnumber(L, buffer_tell(&self->buf));
	return 2;
}

static int lib_insert (lua_State *L)
{
	int i, top = lua_gettop(L);
	struct writer_t W;
	lua_Stream *self = (lua_Stream *)luaL_checkudata(L, 1, LIB_NAME);
	size_t pos = luaL_checkint(L, 2);
	luaL_check(self->buf, "%s (released) #1", LIB_NAME);
	luaL_check(pos <= buffer_tell(&self->buf), "out of range #2");
	W.count = 0;
	if (pos < buffer_tell(&self->buf))
	{
		buffer_t buf = buffer_new(BUFF_SIZE);
		for (i = 3, W.pos = 0; i <= top; ++i, W.pos = buffer_tell(&buf))
			buffer_writeobject(L, &buf, i, &W);
		buffer_insert(&self->buf, pos, buffer_ptr(&buf), buffer_tell(&buf));
		buffer_delete(&buf);
	}
	else for (i = 3, W.pos = pos; i <= top; ++i, W.pos = buffer_tell(&self->buf))
		buffer_writeobject(L, &self->buf, i, &W);
	lua_pushnumber(L, pos);
	lua_pushnumber(L, buffer_tell(&self->buf));
	return 2;
}

static int lib_read (lua_State *L)
{
	struct reader_t R;
	lua_Stream *self = (lua_Stream *)luaL_checkudata(L, 1, LIB_NAME);
	size_t i, pos, nb = luaL_optint(L, 2, 1);
	luaL_check(self->buf, "%s (released) #1", LIB_NAME);
	R.count = 0;
	R.pos = self->pos;
	for (i = 0; i < nb; ++i, R.pos = self->pos)
		self->pos = buffer_readobject(L, buffer_ptr(&self->buf), self->pos, buffer_tell(&self->buf), &R);
	for (i = 0; i < R.count; ++i)
		luaL_unref(L, LUA_REGISTRYINDEX, R.refs[i].idx);
	return nb;
}

static int lib_remove (lua_State *L)
{
	int top = lua_gettop(L);
	lua_Stream *self = (lua_Stream *)luaL_checkudata(L, 1, LIB_NAME);
	size_t size, pos;
	luaL_check(self->buf, "%s (released) #1", LIB_NAME);
	pos = luaL_checkint(L, 2);
	size = luaL_checkint(L, 3);
	stream_remove(self, pos, size);
	return 0;
}

static int lib_seek (lua_State *L)
{
	lua_Stream *self = (lua_Stream *)luaL_checkudata(L, 1, LIB_NAME);
	size_t pos = luaL_checkint(L, 2);
	luaL_check(self->buf, "%s (released) #1", LIB_NAME);
	luaL_check(pos <= buffer_tell(&self->buf), "out of range #2");
	self->pos = pos;
	return 0;
}

static int lib_tell (lua_State *L)
{
	lua_Stream *self = (lua_Stream *)luaL_checkudata(L, 1, LIB_NAME);
	luaL_check(self->buf, "%s (released) #1", LIB_NAME);
	lua_pushnumber(L, self->pos);
	return 1;
}

static int lib_unread (lua_State *L)
{
	lua_Stream *self = (lua_Stream *)luaL_checkudata(L, 1, LIB_NAME);
	luaL_check(self->buf, "%s (released) #1", LIB_NAME);
	lua_pushnumber(L, buffer_tell(&self->buf) - self->pos);
	return 1;
}

static int lib_size (lua_State *L)
{
	lua_Stream *self = (lua_Stream *)luaL_checkudata(L, 1, LIB_NAME);
	luaL_check(self->buf, "%s (released) #1", LIB_NAME);
	lua_pushnumber(L, buffer_tell(&self->buf));
	return 1;
}

static int lib_empty (lua_State *L)
{
	lua_Stream *self = (lua_Stream *)luaL_checkudata(L, 1, LIB_NAME);
	if (!self->buf)
		lua_pushnil(L);
	else
		lua_pushboolean(L, buffer_tell(&self->buf) == 0);
	return 1;
}

static int lib_eof (lua_State *L)
{
	lua_Stream *self = (lua_Stream *)luaL_checkudata(L, 1, LIB_NAME);
	luaL_check(self->buf, "%s (released) #1", LIB_NAME);
	lua_pushboolean(L, self->pos >= buffer_tell(&self->buf));
	return 1;
}

static const char *getnext(const char *f, const char *e)
{
	while(f < e && isdigit(*f)) ++f;
	return f;
}

static size_t getsize(const char *f, const char *e)
{
	if (f < e)
		return atoi(f);
	return 0;
}

static int lib_writef (lua_State *L)
{
	lua_Stream *self = (lua_Stream *)luaL_checkudata(L, 1, LIB_NAME);
	size_t len, pos, i = 2;
	const char *f, *e;
	f = luaL_checklstring(L, 2, &len);
	e = f + len;
	luaL_check(self->buf, "%s (released) #1", LIB_NAME);
	pos = buffer_tell(&self->buf);
	for (; f < e; f = getnext(f, e))
	{
		switch(*(f++))
		{
		case F_SIGNED_BYTE:
			{
				char n = luaL_checkinteger(L, ++i);
				correctbytes(&n, sizeof(n));
				buffer_write(&self->buf, &n, sizeof(n));
				break;
			}
		case F_UNSIGNED_BYTE:
			{
				unsigned char n = luaL_checkinteger(L, ++i);
				correctbytes(&n, sizeof(n));
				buffer_write(&self->buf, &n, sizeof(n));
				break;
			}
		case F_SIGNED_WORD:
			{
				short n = luaL_checkinteger(L, ++i);
				correctbytes(&n, sizeof(n));
				buffer_write(&self->buf, &n, sizeof(n));
				break;
			}
		case F_UNSIGNED_WORD:
			{
				unsigned short n = luaL_checkinteger(L, ++i);
				correctbytes(&n, sizeof(n));
				buffer_write(&self->buf, &n, sizeof(n));
				break;
			}
		case F_SIGNED_DWORD:
			{
				long n = luaL_checkinteger(L, ++i);
				correctbytes(&n, sizeof(n));
				buffer_write(&self->buf, &n, sizeof(n));
				break;
			}
		case F_UNSIGNED_DWORD:
			{
				unsigned long n = luaL_checkinteger(L, ++i);
				correctbytes(&n, sizeof(n));
				buffer_write(&self->buf, &n, sizeof(n));
				break;
			}
		case F_FLOAT:
			{
				lua_Number n = luaL_checknumber(L, ++i);
				correctbytes(&n, sizeof(n));
				buffer_write(&self->buf, &n, sizeof(n));
				break;
			}
		case F_ZSTRING:
			{
				const char *s = luaL_checklstring(L, ++i, &len);
				size_t sz = getsize(f, e);
				if (sz == 0) sz = len;
				buffer_write(&self->buf, s, sz);
				buffer_writebyte(&self->buf, 0);
				break;
			}
		case F_STRING:
			{
				const char *s = luaL_checklstring(L, ++i, &len);
				size_t sz = getsize(f, e);
				if (sz == 0) sz = len;
				buffer_write(&self->buf, s, sz);
				break;
			}
		case F_OBJECT:
			{
				struct writer_t W;
				W.count = 0;
				W.pos = buffer_tell(&self->buf);
				buffer_writeobject(L, &self->buf, ++i, &W);
				break;
			}
		default:
			luaL_error(L, "unsupport format '%c'", *(--f));
			break;
		}
	}
	lua_pushnumber(L, pos);
	lua_pushnumber(L, buffer_tell(&self->buf));
	return 2;
}

static int lib_insertf (lua_State *L)
{
	lua_Stream *self = (lua_Stream *)luaL_checkudata(L, 1, LIB_NAME);
	size_t len, i = 3, pos = luaL_checkint(L, 2);
	const char *f, *e;
	f = luaL_checklstring(L, 3, &len);
	e = f + len;
	luaL_check(self->buf, "%s (released) #1", LIB_NAME);
	luaL_check(pos <= buffer_tell(&self->buf), "out of range #2");
	for (; f < e; f = getnext(f, e))
	{
		switch(*(f++))
		{
		case F_SIGNED_BYTE:
			{
				char n = luaL_checkinteger(L, ++i);
				correctbytes(&n, sizeof(n));
				buffer_insert(&self->buf, pos, &n, sizeof(n));
				pos += sizeof(n);
				break;
			}
		case F_UNSIGNED_BYTE:
			{
				unsigned char n = luaL_checkinteger(L, ++i);
				correctbytes(&n, sizeof(n));
				buffer_insert(&self->buf, pos, &n, sizeof(n));
				pos += sizeof(n);
				break;
			}
		case F_SIGNED_WORD:
			{
				short n = luaL_checkinteger(L, ++i);
				correctbytes(&n, sizeof(n));
				buffer_insert(&self->buf, pos, &n, sizeof(n));
				pos += sizeof(n);
				break;
			}
		case F_UNSIGNED_WORD:
			{
				unsigned short n = luaL_checkinteger(L, ++i);
				correctbytes(&n, sizeof(n));
				buffer_insert(&self->buf, pos, &n, sizeof(n));
				pos += sizeof(n);
				break;
			}
		case F_SIGNED_DWORD:
			{
				long n = luaL_checkinteger(L, ++i);
				correctbytes(&n, sizeof(n));
				buffer_insert(&self->buf, pos, &n, sizeof(n));
				pos += sizeof(n);
				break;
			}
		case F_UNSIGNED_DWORD:
			{
				unsigned long n = luaL_checkinteger(L, ++i);
				correctbytes(&n, sizeof(n));
				buffer_insert(&self->buf, pos, &n, sizeof(n));
				pos += sizeof(n);
				break;
			}
		case F_FLOAT:
			{
				lua_Number n = luaL_checknumber(L, ++i);
				correctbytes(&n, sizeof(n));
				buffer_insert(&self->buf, pos, &n, sizeof(n));
				pos += sizeof(n);
				break;
			}
		case F_ZSTRING:
			{
				const char *s = luaL_checklstring(L, ++i, &len);
				size_t sz = getsize(f, e);
				if (sz == 0) sz = len;
				buffer_insert(&self->buf, pos, s, sz);
				buffer_insertbyte(&self->buf, pos, 0);
				pos += sz + 1;
				break;
			}
		case F_STRING:
			{
				const char *s = luaL_checklstring(L, ++i, &len);
				size_t sz = getsize(f, e);
				if (sz == 0) sz = len;
				buffer_insert(&self->buf, pos, s, sz);
				pos += sz;
				break;
			}
		case F_OBJECT:
			{
				struct writer_t W;
				W.count = 0;
				if (pos < buffer_tell(&self->buf))
				{
					buffer_t buf = buffer_new(BUFF_SIZE);
					W.pos = buffer_tell(&buf);
					buffer_writeobject(L, &buf, ++i, &W);
					buffer_insert(&self->buf, pos, buffer_ptr(&buf), buffer_tell(&buf));
					pos += buffer_tell(&buf);
					buffer_delete(&buf);
				}
				else
				{
					W.pos = buffer_tell(&self->buf);
					buffer_writeobject(L, &self->buf, ++i, &W);
				}
				break;
			}
		default:
			luaL_error(L, "unsupport format '%c'", *(--f));
			break;
		}
	}
	lua_pushnumber(L, pos);
	lua_pushnumber(L, buffer_tell(&self->buf));
	return 2;
}

static int lib_readf (lua_State *L)
{
	lua_Stream *self = (lua_Stream *)luaL_checkudata(L, 1, LIB_NAME);
	size_t len, nb = 0;
	const char *f, *e;
	f = luaL_checklstring(L, 2, &len);
	e = f + len;
	luaL_check(self->buf, "%s (released) #1", LIB_NAME);
	for (; f < e; f = getnext(f, e))
	{
		switch(*(f++))
		{
		case F_SIGNED_BYTE:
			{
				char n = 0;
				buffer_read(&self->buf, self->pos, &n, sizeof(n));
				correctbytes(&n, sizeof(n));
				lua_pushnumber(L, n);
				self->pos += sizeof(n);
				break;
			}
		case F_UNSIGNED_BYTE:
			{
				unsigned char n = 0;
				buffer_read(&self->buf, self->pos, &n, sizeof(n));
				correctbytes(&n, sizeof(n));
				lua_pushnumber(L, n);
				self->pos += sizeof(n);
				break;
			}
		case F_SIGNED_WORD:
			{
				short n = 0;
				buffer_read(&self->buf, self->pos, &n, sizeof(n));
				correctbytes(&n, sizeof(n));
				lua_pushnumber(L, n);
				self->pos += sizeof(n);
				break;
			}
		case F_UNSIGNED_WORD:
			{
				unsigned short n = 0;
				buffer_read(&self->buf, self->pos, &n, sizeof(n));
				correctbytes(&n, sizeof(n));
				lua_pushnumber(L, n);
				self->pos += sizeof(n);
				break;
			}
		case F_SIGNED_DWORD:
			{
				long n = 0;
				buffer_read(&self->buf, self->pos, &n, sizeof(n));
				correctbytes(&n, sizeof(n));
				lua_pushnumber(L, n);
				self->pos += sizeof(n);
				break;
			}
		case F_UNSIGNED_DWORD:
			{
				unsigned long n = 0;
				buffer_read(&self->buf, self->pos, &n, sizeof(n));
				correctbytes(&n, sizeof(n));
				lua_pushnumber(L, n);
				self->pos += sizeof(n);
				break;
			}
		case F_FLOAT:
			{
				lua_Number n = 0;
				buffer_read(&self->buf, self->pos, &n, sizeof(n));
				correctbytes(&n, sizeof(n));
				lua_pushnumber(L, n);
				self->pos += sizeof(n);
				break;
			}
		case F_ZSTRING:
			{
				const char *s = buffer_at(&self->buf, self->pos);
				size_t sz = strlen(s);
				lua_pushlstring(L, s, sz);
				self->pos += sz + 1;
				break;
			}
		case F_STRING:
			{
				const char *s = buffer_at(&self->buf, self->pos);
				size_t sz = getsize(f, e);
				lua_pushlstring(L, s, sz);
				self->pos += sz;
				break;
			}
		case F_OBJECT:
			{
				size_t i;
				struct reader_t R;
				R.count = 0;
				R.pos = self->pos;
				self->pos = buffer_readobject(L, buffer_ptr(&self->buf), self->pos, buffer_tell(&self->buf), &R);
				for (i = 0; i < R.count; ++i)
					luaL_unref(L, LUA_REGISTRYINDEX, R.refs[i].idx);
				break;
			}
		default:
			luaL_error(L, "unsupport format '%c'", *(--f));
			break;
		}
		++nb;
	}
	return nb;
}

static const struct luaL_Reg lib[] = {
	{"new", lib_new},
	{"clone", lib_clone},
	{"write", lib_write},
	{"insert", lib_insert},
	{"read", lib_read},
	{"remove", lib_remove},
	{"seek", lib_seek},
	{"tell", lib_tell},
	{"unread", lib_unread},
	{"size", lib_size},
	{"empty", lib_empty},
	{"eof", lib_eof},
	{"writef", lib_writef},
	{"insertf", lib_insertf},
	{"readf", lib_readf},
	{"tostring", lib_tostring},
	{"release", lib_release},
	{"writefrom", lib_writefrom},
	{"insertfrom", lib_insertfrom},
	{"__gc", lib_release},
	{"__tostring", lib_mt_tostring},
	{"__len", lib_size},
	{NULL, NULL}
};

LUALIB_API int luaopen_stream (lua_State *L)
{
	luaL_newmetatable(L, LIB_NAME);
	lua_pushvalue(L, -1);
	lua_setfield(L, -2, "__index");
	luaL_register(L, NULL, lib);
	
	lua_pushstring(L, "VERSION");
	lua_pushstring(L, VERSION);
	lua_settable(L, -3);
	
	lua_pushstring(L, "RELEASE");
	lua_pushstring(L, RELEASE);
	lua_settable(L, -3);
	
	lua_pushstring(L, "AUTHORS");
	lua_pushstring(L, AUTHORS);
	lua_settable(L, -3);
	return 1;
}

lua_Stream *stream_new(lua_State *L)
{
	lua_Stream *self = (lua_Stream *)lua_newuserdata(L, sizeof(lua_Stream));
	self->buf = buffer_new(BUFF_SIZE);
	self->pos = 0;
	luaL_getmetatable(L, LIB_NAME);
	lua_setmetatable(L, -2);
	self->ref = luaL_ref(L, LUA_REGISTRYINDEX);
}

lua_Stream *stream_ref(lua_State *L, int index)
{
	lua_Stream *self;
	if (lua_isnoneornil(L, index))
		return NULL;
	self = (lua_Stream *)luaL_checkudata(L, index, LIB_NAME);
	lua_pushvalue(L, index);
	self->ref = luaL_ref(L, LUA_REGISTRYINDEX);
	return self;
}

void stream_unref(lua_State *L, lua_Stream *self)
{
	if (self->ref != LUA_REFNIL)
	{
		luaL_unref(L, LUA_REGISTRYINDEX, self->ref);
		self->ref = LUA_REFNIL;
	}
}

void stream_push(lua_State *L, lua_Stream *self)
{
	if (self)
		lua_rawgeti(L, LUA_REGISTRYINDEX, self->ref);
	else
		lua_pushnil(L);
}

size_t stream_tell(lua_Stream *self)
{
	return self->pos;
}

size_t stream_size(lua_Stream *self)
{
	return buffer_tell(&self->buf);
}

void stream_write(lua_Stream *self, const void *data, size_t size)
{
	buffer_write(&self->buf, data, size);
}

void stream_read(lua_Stream *self, size_t pos, void *data, size_t size)
{
	assert(pos + size <= stream_size(self));
	buffer_read(&self->buf, pos, data, size);
}

void stream_insert(lua_Stream *self, size_t pos, const void *data, size_t size)
{
	assert(pos <= stream_size(self));
	buffer_insert(&self->buf, pos, data, size);
}

void stream_remove(lua_Stream *self, size_t pos, size_t size)
{
	assert(pos + size <= stream_size(self));
	buffer_remove(&self->buf, pos, size);
	if (self->pos >= pos + size) self->pos -= size;
	else if(self->pos >= pos) self->pos = pos;
}

char *stream_ptr(lua_Stream *self)
{
	return buffer_ptr(&self->buf);
}
