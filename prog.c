#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>

static void trim_trailing_newline(char *str)
{
	size_t len = strlen(str);

	if (len > 0 && str[len - 1] == '\n') {
		str[len - 1] = 0;
	}
}

/*
 * Returns -1 on error (typically EOF).
 */
static int read_line(char *s, size_t size, FILE *infile)
{
	while (1) {
		if (fgets(s, size, infile) == NULL)
			return -1;

		trim_trailing_newline(s);

		/*
		 * Ignore empty lines and lines starting with #
		 * characters for the benefit of the test harness.
		 */
		if (s[0] != 0 && s[0] != '#')
			break;
	}

	return 0;
}

static int parse_hex_nibble(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';

	if (c >= 'A' && c <= 'F')
		return 10 + (c - 'A');

	if (c >= 'a' && c <= 'f')
		return 10 + (c - 'a');

	return -1;
}

static ssize_t parse_hex_string(uint8_t *buf, size_t buf_len, const char *str)
{
	size_t str_len = strlen(str);

	if ((str_len % 2) == 1) {
		printf("Error: hex string [%s] has an odd number of "
		       "nibbles\n", str);
		return -1;
	}

	size_t msg_len = str_len / 2;

	if (msg_len > buf_len) {
		printf("Buffer of size %zd too small for message "
		       "of size %zd\n", buf_len, msg_len);
		return -1;
	}

	for (size_t i = 0; i < msg_len; i++) {
		int upper = parse_hex_nibble(str[2 * i]);

		if (upper < 0) {
			printf("Error: invalid hex nibble '%c' seen\n", upper);
			return -1;
		}

		int lower = parse_hex_nibble(str[2 * i + 1]);

		if (lower < 0) {
			printf("Error: invalid hex nibble '%c' seen\n", lower);
			return -1;
		}

		buf[i] = (upper << 4) | lower;
	}

	return msg_len;
}

#define MSG_HEADER_BYTES		2
#define MSG_MIN_PAYLOAD_LENGTH		4
#define MSG_MAX_PAYLOAD_LENGTH		255

struct message {
	uint8_t		type;
	uint8_t		len;	/* including CRC, which is not part of data */
	uint8_t		data[MSG_MAX_PAYLOAD_LENGTH];
	uint32_t	crc;
};

#define MSG_BUF_SIZE			519

/*
 * Accommodate:
 * - 4 bytes for "msg="
 * - 2 hex characters per byte
 *   - type byte, length byte, and up to 256 payload bytes
 * - 1 for trailing NUL byte
 */
_Static_assert(
	MSG_BUF_SIZE >=
		4 + (2 * (MSG_HEADER_BYTES + MSG_MAX_PAYLOAD_LENGTH)) + 1,
	"MSG_BUF_SIZE too small"
);

/*
 * Returns -1 on error.
 */
static int parse_message(struct message *dst, const char *str)
{
	size_t len = strlen(str);

	if (len < 4 || memcmp(str, "msg=", 4)) {
		printf("Error: message line [%s] doesn't start with "
		       "msg=\n", str);
		exit(EXIT_FAILURE);
	}

	const char *msg_ptr = str + 4;
	size_t msg_nibbles = len - 4;

	if ((msg_nibbles % 2) == 1) {
		printf("Error: message [%s] has an odd number of "
		       "nibbles\n", str);
		return -1;
	}

	size_t msg_bytes = msg_nibbles / 2;

	if (msg_bytes < MSG_HEADER_BYTES + MSG_MIN_PAYLOAD_LENGTH) {
		printf("Error: message [%s] is too short\n", str);
		return -1;
	}

	if (msg_bytes > MSG_HEADER_BYTES + MSG_MAX_PAYLOAD_LENGTH) {
		printf("Error: message [%s] is too long\n", str);
		return -1;
	}

	uint8_t msg[MSG_HEADER_BYTES + MSG_MAX_PAYLOAD_LENGTH];

	size_t parsed_msg_bytes = parse_hex_string(msg, sizeof(msg), msg_ptr);
	if (parsed_msg_bytes < 0)
		return -1;

	if (msg_bytes != parsed_msg_bytes) {
		printf("Internal error: message of length %zd "
		       "became %zd bytes after parsing\n", msg_bytes,
		       parsed_msg_bytes);
		return -1;
	}

	if (msg[1] != msg_bytes - 2) {
		printf("Error: message is %zd bytes long but has a "
		       "length field of %d\n", msg_bytes - 2, msg[1]);
		return -1;
	}

	dst->type = msg[0];
	dst->len = msg[1];
	memcpy(dst->data, msg + 2, msg_bytes - 6);
	dst->crc = (((uint32_t)msg[msg_bytes - 4]) << 24) |
		(((uint32_t)msg[msg_bytes - 3]) << 16) |
		(((uint32_t)msg[msg_bytes - 2]) << 8) |
		((uint32_t)msg[msg_bytes - 1]);

	return 0;
}

#define MASK_BUF_SIZE		16

/*
 * Accommodate:
 * - 5 bytes or "mask="
 * - 8 bytes for a 32-bit hexadecimal mask value
 * - 1 for trailing NUL byte
 */
_Static_assert(MASK_BUF_SIZE >= 5 + 8 + 1);

/*
 * Returns -1 on error.
 */
static int parse_mask(uint32_t *maskp, const char *str)
{
	size_t len = strlen(str);

	if (len < 5 || memcmp(str, "mask=", 5)) {
		printf("Error: mask line [%s] doesn't start with mask=\n",
		       str);
		return -1;
	}

	const char *mask_ptr = str + 5;

	unsigned long mask;

	if (sscanf(mask_ptr, "%lx", &mask) != 1) {
		printf("Error: unable to parse mask [%s]\n", mask_ptr);
		return -1;
	}

	if (mask > 0xffffffff) {
		printf("Error: mask %lx does not fit in 32 bits\n", mask);
		return -1;
	}

	*maskp = (uint32_t)mask;

	return 0;
}

static uint32_t crc32(const uint8_t *data, size_t data_len)
{
	uint32_t crc = 0xffffffff;

	for (size_t i = 0; i < data_len; i++) {
		for (int bit = 0; bit < 8; bit++) {
			uint32_t b = (data[i] >> bit) & 1;

			if ((crc ^ b) & 1) {
				crc = (crc >> 1) ^ 0xedb88320;
			} else {
				crc = crc >> 1;
			}
		}
	}

	crc ^= 0xffffffff;

	return crc;
}

static uint32_t compute_message_crc(const struct message *msg)
{
	return crc32(msg->data, msg->len - 4);
}

static void print_hex(const uint8_t *buf, size_t len)
{
	for (size_t i = 0; i < len; i++) {
		if (i != 0)
			printf(" ");
		printf("%.2x", buf[i]);
	}
	printf("\n");
}

static int transform_message(struct message *msg, uint32_t mask)
{
	/* Pad message to a multiple of 4 bytes  */
	if (msg->len > 252) {
		printf("Error: can't pad message of length %d to a multiple "
		       "of 4 bytes, since the maximum message length is 255 "
		       "bytes\n", msg->len);
		return -1;
	}

	while ((msg->len % 4) != 0) {
		msg->data[msg->len - 4] = 0;
		msg->len++;
	}

	/* Apply mask to even words  */
	for (size_t i = 0; i < msg->len - 4; i++) {
		uint8_t mask_byte;

		switch (i & 7) {
		case 0: mask_byte = (mask >> 24) & 0xff; break;
		case 1: mask_byte = (mask >> 16) & 0xff; break;
		case 2: mask_byte = (mask >> 8) & 0xff; break;
		case 3: mask_byte = mask & 0xff; break;
		default: mask_byte = 0xff; break;
		}

		msg->data[i] &= mask_byte;
	}

	/* Recompute CRC  */
	msg->crc = compute_message_crc(msg);

	return 0;
}

int main()
{
	FILE *infile;

	if (freopen("data_out.txt", "w", stdout) == NULL) {
		perror("Error opening data_out.txt");
		return 1;
	}

	infile = fopen("data_in.txt", "r");
	if (infile == NULL) {
		printf("Error opening data_in.txt: %s\n", strerror(errno));
		return 1;
	}

	while (1) {
		char msg_line[MSG_BUF_SIZE];
		char mask_line[MASK_BUF_SIZE];
		struct message msg;
		uint32_t mask;

		if (read_line(msg_line, sizeof(msg_line), infile) < 0)
			break;

		if (read_line(mask_line, sizeof(mask_line), infile) < 0)
			break;

		int message_ret = parse_message(&msg, msg_line);
		int mask_ret = parse_mask(&mask, mask_line);

		if (message_ret == 0 && mask_ret == 0) {
			uint32_t computed_crc = compute_message_crc(&msg);

			if (msg.crc == computed_crc) {
				struct message msg2 = msg;

				if (transform_message(&msg2, mask) == 0) {
					printf("%.2x\n", msg.type);

					printf("%.2x\n", msg.len);
					print_hex(msg.data, msg.len - 4);
					printf("%.8x\n", msg.crc);

					printf("%.2x\n", msg2.len);
					print_hex(msg2.data, msg2.len - 4);
					printf("%.8x\n", msg2.crc);
				}
			} else {
				printf("Error: message has CRC %.8x but "
				       "expected CRC %.8x\n", msg.crc,
				       computed_crc);
			}
		}

		printf("\n");
	}

	fclose(infile);

	return 0;
}
