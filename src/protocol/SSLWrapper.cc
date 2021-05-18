/*
  Copyright (c) 2021 Sogou, Inc.

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

      http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.

  Author: Xie Han (xiehan@sogou-inc.com)
*/

#include <errno.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/bio.h>
#include "SSLWrapper.h"

namespace protocol
{

int SSLHandshaker::encode(struct iovec vectors[], int max)
{
	BIO *wbio = SSL_get_wbio(this->ssl);
	char *ptr;
	long len;
	int ret;

	if (BIO_reset(wbio) <= 0)
		return -1;

	ret = SSL_do_handshake(this->ssl);
	if (ret <= 0)
	{
		ret = SSL_get_error(this->ssl, ret);
		if (ret != SSL_ERROR_WANT_READ)
		{
			if (ret != SSL_ERROR_SYSCALL)
				errno = -ret;

			return -1;
		}
	}

	len = BIO_get_mem_data(wbio, &ptr);
	if (len > 0)
	{
		vectors[0].iov_base = ptr;
		vectors[0].iov_len = len;
		return 1;
	}
	else if (len == 0)
		return 0;
	else
		return -1;
}

static int __ssl_handshake(const void *buf, size_t *size, SSL *ssl,
						   char **ptr, long *len)
{
	BIO *wbio = SSL_get_wbio(ssl);
	BIO *rbio = SSL_get_rbio(ssl);
	int ret;

	if (BIO_reset(wbio) <= 0)
		return -1;

	ret = BIO_write(rbio, buf, *size);
	if (ret <= 0)
		return -1;

	*size = ret;
	ret = SSL_do_handshake(ssl);
	if (ret <= 0)
	{
		ret = SSL_get_error(ssl, ret);
		if (ret != SSL_ERROR_WANT_READ)
		{
			if (ret != SSL_ERROR_SYSCALL)
				errno = -ret;

			return -1;
		}

		ret = 0;
	}

	*len = BIO_get_mem_data(wbio, ptr);
	if (*len < 0)
		return -1;

	return ret;
}

int SSLHandshaker::append(const void *buf, size_t *size)
{
	char *ptr;
	long len;
	long n;
	int ret;

	ret = __ssl_handshake(buf, size, this->ssl, &ptr, &len);
	if (ret < 0)
		return -1;

	if (len > 0)
		n = this->feedback(ptr, len);
	else
		n = 0;

	if (n == len)
		return ret;

	if (n >= 0)
		errno = EAGAIN;

	return -1;
}

int SSLWrapper::encode(struct iovec vectors[], int max)
{
	BIO *wbio = SSL_get_wbio(this->ssl);
	struct iovec *iov;
	char *ptr;
	long len;
	int ret;

	if (BIO_reset(wbio) <= 0)
		return -1;

	ret = this->msg->encode(vectors, max);
	if ((unsigned int)ret > (unsigned int)max)
		return ret;

	max = ret;
	for (iov = vectors; iov < vectors + max; iov++)
	{
		if (iov->iov_len > 0)
		{
			ret = SSL_write(this->ssl, iov->iov_base, iov->iov_len);
			if (ret <= 0)
			{
				ret = SSL_get_error(this->ssl, ret);
				if (ret != SSL_ERROR_SYSCALL)
					errno = -ret;

				return -1;
			}
		}
	}

	len = BIO_get_mem_data(wbio, &ptr);
	if (len > 0)
	{
		vectors[0].iov_base = ptr;
		vectors[0].iov_len = len;
		return 1;
	}
	else if (len == 0)
		return 0;
	else
		return -1;
}

#define BUFSIZE		8192

int SSLWrapper::append_message()
{
	char buf[BUFSIZE];
	int ret;

	while ((ret = SSL_read(this->ssl, buf, BUFSIZE)) > 0)
	{
		size_t nleft = ret;
		char *p = buf;
		size_t n;

		do
		{
			n = nleft;
			ret = this->msg->append(p, &n);
			if (ret == 0)
			{
				nleft -= n;
				p += n;
			}
			else
				return ret;

		} while (nleft > 0);
	}

	if (ret < 0)
	{
		ret = SSL_get_error(this->ssl, ret);
		if (ret != SSL_ERROR_WANT_READ)
		{
			if (ret != SSL_ERROR_SYSCALL)
				errno = -ret;

			return -1;
		}
	}

	return 0;
}

int SSLWrapper::append(const void *buf, size_t *size)
{
	BIO *rbio = SSL_get_rbio(this->ssl);
	int ret;

	ret = BIO_write(rbio, buf, *size);
	if (ret <= 0)
		return -1;

	*size = ret;
	return this->append_message();
}

int ServiceSSLWrapper::append(const void *buf, size_t *size)
{
	char *ptr;
	long len;
	long n;

	if (__ssl_handshake(buf, size, this->ssl, &ptr, &len) < 0)
		return -1;

	if (len > 0)
		n = this->feedback(ptr, len);
	else
		n = 0;

	if (n == len)
		return this->append_message();

	if (n >= 0)
		errno = EAGAIN;

	return -1;
}

}

