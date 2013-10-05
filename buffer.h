
typedef struct buffer_ *buffer_t;

buffer_t buffer_new(size_t size);
void buffer_delete(buffer_t *self);
void buffer_needsize(buffer_t *self, size_t size);
void buffer_checksize(buffer_t *self, size_t size);
void buffer_writebyte(buffer_t *self, char ch);
void buffer_write(buffer_t *self, const void *data, size_t size);
void buffer_read(buffer_t *self, size_t pos, void* data, size_t size);
void buffer_insertbyte(buffer_t *self, size_t pos, char ch);
void buffer_insert(buffer_t *self, size_t pos, const void *data, size_t size);
void buffer_remove(buffer_t *self, size_t pos, size_t size);
size_t buffer_tell(buffer_t *self);
size_t buffer_size(buffer_t *self);
char *buffer_ptr(buffer_t *self);
char *buffer_at(buffer_t *self, size_t pos);

