
#include <math.h>
#include <malloc.h>
#include <assert.h>

#include "buffer.h"

#ifndef MIN
#define MIN(x, y) ((x) < (y) ? x : y)
#endif
#ifndef MAX
#define MAX(x, y) ((x) > (y) ? x : y)
#endif

#define _self (*self)

struct buffer_
{
	size_t pos;
	size_t size;
	char ptr[0];
};

buffer_t buffer_new(size_t size)
{
	buffer_t self = (buffer_t)malloc(sizeof(struct buffer_) + size);
	self->pos = 0;
	self->size = size;
	return self;
}

void buffer_delete(buffer_t *self)
{
	free(_self);
	_self = NULL;
}

void buffer_needsize(buffer_t *self, size_t size)
{
	if (_self->pos + size > _self->size)
	{
		_self->size += MAX(size, _self->size);
		_self = (struct buffer_*)realloc(_self, sizeof(struct buffer_) + _self->size);
	}
}

void buffer_checksize(buffer_t *self, size_t size)
{
	if (size > _self->size)
	{
		_self->size += size;
		_self = (struct buffer_*)realloc(_self, sizeof(struct buffer_) + _self->size);
	}
}

void buffer_writebyte(buffer_t *self, char ch)
{
	buffer_needsize(self, 1);
	_self->ptr[_self->pos++] = ch;
}

void buffer_write(buffer_t *self, const void *ptr, size_t size)
{
	buffer_needsize(self, size);
	memcpy(_self->ptr + _self->pos, ptr, size);
	_self->pos += size;
}

void buffer_read(buffer_t *self, size_t pos, void* data, size_t size)
{
	size_t read = MIN(_self->size - pos, size);
	memcpy(data, _self->ptr + pos, read);
}

size_t buffer_tell(buffer_t *self)
{
	return _self->pos;
}

void buffer_insertbyte(buffer_t *self, size_t pos, char ch)
{
	buffer_insert(self, pos, &ch, sizeof(ch));
}

void buffer_insert(buffer_t *self, size_t pos, const void *data, size_t size)
{
	assert(pos <= _self->pos);
	buffer_needsize(self, size);
	memmove(_self->ptr + pos + size, _self->ptr + pos, _self->pos - pos);
	memcpy(_self->ptr + pos, data, size);
	if (_self->pos >= pos) _self->pos += size;
}

void buffer_remove(buffer_t *self, size_t pos, size_t size)
{
	assert(pos + size <= _self->pos);
	memmove(_self->ptr + pos, _self->ptr + pos + size, _self->pos - pos - size);
	_self->pos -= size;
}

size_t buffer_size(buffer_t *self)
{
	return _self->size;
}

char *buffer_ptr(buffer_t *self)
{
	return _self->ptr;
}

char *buffer_at(buffer_t *self, size_t pos)
{
	assert(pos < _self->pos);
	return _self->ptr + pos;
}
