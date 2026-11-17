#include <stdint.h>
typedef uint64_t usize_t;
int xd3_decode_memory(const uint8_t *input, usize_t input_size, const uint8_t *source, usize_t source_size, uint8_t *output_buf, usize_t *output_size, usize_t avail_output, int flags);
