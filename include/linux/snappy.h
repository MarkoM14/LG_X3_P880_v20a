#ifndef _LINUX_SNAPPY_H
#define _LINUX_SNAPPY_H 1

#include <stdbool.h>
#include <stddef.h>

#if defined(__arm__) && \
	!defined(__ARM_ARCH_4__) &&		\
	!defined(__ARM_ARCH_4T__) &&		\
	!defined(__ARM_ARCH_5__) &&		\
	!defined(__ARM_ARCH_5T__) &&		\
	!defined(__ARM_ARCH_5TE__) &&		\
	!defined(__ARM_ARCH_5TEJ__) &&		\
	!defined(__ARM_ARCH_6__) &&		\
	!defined(__ARM_ARCH_6J__) &&		\
	!defined(__ARM_ARCH_6K__) &&		\
	!defined(__ARM_ARCH_6Z__) &&		\
	!defined(__ARM_ARCH_6ZK__) &&		\
	!defined(__ARM_ARCH_6T2__)
#define  UNALIGNED64_REALLYS_SLOW 1
#endif

/* Only needed for compression. This preallocates the worst case */
struct snappy_env {
	unsigned short *hash_table;
	void *scratch;
	void *scratch_output;
};

struct iovec;
int snappy_init_env(struct snappy_env *env);
int snappy_init_env_sg(struct snappy_env *env, bool sg);
void snappy_free_env(struct snappy_env *env);
int snappy_uncompress_iov(struct iovec *iov_in, int iov_in_len,
			   size_t input_len, char *uncompressed);
int snappy_uncompress(const char *compressed, size_t n, char *uncompressed);
int snappy_compress(struct snappy_env *env,
		    const char *input,
		    size_t input_length,
		    char *compressed,
		    size_t *compressed_length);
int snappy_compress_iov(struct snappy_env *env,
			struct iovec *iov_in,
			int iov_in_len,
			size_t input_length,
			struct iovec *iov_out,
			int *iov_out_len,
			size_t *compressed_length);
bool snappy_uncompressed_length(const char *buf, size_t len, size_t *result);
size_t snappy_max_compressed_length(size_t source_len);

/* You may want to define this on various ARM architectures */
#ifdef UNALIGNED64_REALLYS_SLOW
static inline u64 get_unaligned64(const void *p)
{
	u64 t;
	memcpy(&t, p, 8);
	return t;
}
static inline u64 put_unaligned64(u64 t, void *p)
{
	memcpy(p, &t, 8);
	return t;
}
#else
#define get_unaligned64(x) get_unaligned(x)
#define put_unaligned64(x,p) put_unaligned(x,p)
#endif


#endif