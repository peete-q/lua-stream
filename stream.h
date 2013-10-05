
#include "lua.h"
#include "lauxlib.h"

typedef struct lua_Stream lua_Stream;

lua_Stream *stream_new(lua_State *L);
lua_Stream *stream_ref(lua_State *L, int index);
void stream_unref(lua_State *L, lua_Stream *self);
void stream_push(lua_State *L, lua_Stream *self);
size_t stream_tell(lua_Stream *self);
size_t stream_size(lua_Stream *self);
void stream_write(lua_Stream *self, const void *data, size_t size);
void stream_read(lua_Stream *self, size_t pos, void *data, size_t size);
void stream_insert(lua_Stream *self, size_t pos, const void *data, size_t size);
void stream_remove(lua_Stream *self, size_t pos, size_t size);
char *stream_ptr(lua_Stream *self);