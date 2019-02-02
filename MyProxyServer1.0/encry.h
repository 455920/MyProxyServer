#ifndef __ENCRY_H__
#define __ENCRY_H__

#include <stdio.h>
static inline char* XOR(char* buf, size_t len)
{
	for (size_t i = 0; i < len; ++i)
	{
		buf[i] ^= 9;
	}
}

static inline void Decrypt(char* buf, size_t len)
{
	XOR(buf, len);
}

static inline void Encry(char* buf, size_t len)
{
	XOR(buf, len);
}

#endif //__ENCRY_H__
