/*
 * Copyright (c) 2018 Cisco and/or its affiliates.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <openssl/ssl.h>
#include <openssl/conf.h>
#include <openssl/err.h>
#include <vnet/plugin/plugin.h>
#include <vpp/app/version.h>
#include <vnet/tls/tls.h>

typedef struct tls_ctx_openssl_
{
  tls_ctx_t ctx;			/**< First */
  u32 openssl_ctx_index;
  SSL_CTX *ssl_ctx;
  SSL *ssl;
  BIO *rbio;
  BIO *wbio;
  X509 *srvcert;
  EVP_PKEY *pkey;
} openssl_ctx_t;

typedef struct openssl_main_
{
  openssl_ctx_t ***ctx_pool;
  X509_STORE *cert_store;
} openssl_main_t;

static openssl_main_t openssl_main;

static u32
openssl_ctx_alloc (void)
{
  u8 thread_index = vlib_get_thread_index ();
  openssl_main_t *tm = &openssl_main;
  openssl_ctx_t **ctx;

  pool_get (tm->ctx_pool[thread_index], ctx);
  if (!(*ctx))
    *ctx = clib_mem_alloc (sizeof (openssl_ctx_t));

  memset (*ctx, 0, sizeof (openssl_ctx_t));
  (*ctx)->ctx.c_thread_index = thread_index;
  (*ctx)->ctx.tls_ctx_engine = TLS_ENGINE_OPENSSL;
  (*ctx)->ctx.app_session_handle = SESSION_INVALID_HANDLE;
  (*ctx)->openssl_ctx_index = ctx - tm->ctx_pool[thread_index];
  return ((*ctx)->openssl_ctx_index);
}

static void
openssl_ctx_free (tls_ctx_t * ctx)
{
  openssl_ctx_t *oc = (openssl_ctx_t *) ctx;

  if (SSL_is_init_finished (oc->ssl) && !ctx->is_passive_close)
    SSL_shutdown (oc->ssl);

  if (SSL_is_server (oc->ssl))
    {
      X509_free (oc->srvcert);
      EVP_PKEY_free (oc->pkey);
    }
  SSL_free (oc->ssl);

  pool_put_index (openssl_main.ctx_pool[ctx->c_thread_index],
		  oc->openssl_ctx_index);
}

static tls_ctx_t *
openssl_ctx_get (u32 ctx_index)
{
  openssl_ctx_t **ctx;
  ctx = pool_elt_at_index (openssl_main.ctx_pool[vlib_get_thread_index ()],
			   ctx_index);
  return &(*ctx)->ctx;
}

static tls_ctx_t *
openssl_ctx_get_w_thread (u32 ctx_index, u8 thread_index)
{
  openssl_ctx_t **ctx;
  ctx = pool_elt_at_index (openssl_main.ctx_pool[thread_index], ctx_index);
  return &(*ctx)->ctx;
}

static int
openssl_try_handshake_read (openssl_ctx_t * oc,
			    stream_session_t * tls_session)
{
  u32 deq_max, deq_now;
  svm_fifo_t *f;
  int wrote, rv;

  f = tls_session->server_rx_fifo;
  deq_max = svm_fifo_max_dequeue (f);
  if (!deq_max)
    return 0;

  deq_now = clib_min (svm_fifo_max_read_chunk (f), deq_max);
  wrote = BIO_write (oc->wbio, svm_fifo_head (f), deq_now);
  if (wrote <= 0)
    return 0;

  svm_fifo_dequeue_drop (f, wrote);
  if (wrote < deq_max)
    {
      deq_now = clib_min (svm_fifo_max_read_chunk (f), deq_max - wrote);
      rv = BIO_write (oc->wbio, svm_fifo_head (f), deq_now);
      if (rv > 0)
	{
	  svm_fifo_dequeue_drop (f, rv);
	  wrote += rv;
	}
    }
  return wrote;
}

static int
openssl_try_handshake_write (openssl_ctx_t * oc,
			     stream_session_t * tls_session)
{
  u32 enq_max, deq_now;
  svm_fifo_t *f;
  int read, rv;

  if (BIO_ctrl_pending (oc->rbio) <= 0)
    return 0;

  f = tls_session->server_tx_fifo;
  enq_max = svm_fifo_max_enqueue (f);
  if (!enq_max)
    return 0;

  deq_now = clib_min (svm_fifo_max_write_chunk (f), enq_max);
  read = BIO_read (oc->rbio, svm_fifo_tail (f), deq_now);
  if (read <= 0)
    return 0;

  svm_fifo_enqueue_nocopy (f, read);
  tls_add_vpp_q_evt (f, FIFO_EVENT_APP_TX);

  if (read < enq_max)
    {
      deq_now = clib_min (svm_fifo_max_write_chunk (f), enq_max - read);
      rv = BIO_read (oc->rbio, svm_fifo_tail (f), deq_now);
      if (rv > 0)
	{
	  svm_fifo_enqueue_nocopy (f, rv);
	  read += rv;
	}
    }

  return read;
}

static int
openssl_ctx_handshake_rx (tls_ctx_t * ctx, stream_session_t * tls_session)
{
  openssl_ctx_t *oc = (openssl_ctx_t *) ctx;
  int rv = 0, err;
  while (SSL_in_init (oc->ssl))
    {
      if (!openssl_try_handshake_read (oc, tls_session))
	break;

      rv = SSL_do_handshake (oc->ssl);
      err = SSL_get_error (oc->ssl, rv);
      openssl_try_handshake_write (oc, tls_session);
      if (err != SSL_ERROR_WANT_WRITE)
	{
	  if (err == SSL_ERROR_SSL)
	    {
	      char buf[512];
	      ERR_error_string (ERR_get_error (), buf);
	      clib_warning ("Err: %s", buf);
	    }
	  break;
	}
    }
  TLS_DBG (2, "tls state for %u is %s", oc->openssl_ctx_index,
	   SSL_state_string_long (oc->ssl));

  if (SSL_in_init (oc->ssl))
    return 0;

  /*
   * Handshake complete
   */
  if (!SSL_is_server (oc->ssl))
    {
      /*
       * Verify server certificate
       */
      if ((rv = SSL_get_verify_result (oc->ssl)) != X509_V_OK)
	{
	  TLS_DBG (1, " failed verify: %s\n",
		   X509_verify_cert_error_string (rv));

	  /*
	   * Presence of hostname enforces strict certificate verification
	   */
	  if (ctx->srv_hostname)
	    {
	      tls_notify_app_connected (ctx, /* is failed */ 0);
	      return -1;
	    }
	}
      tls_notify_app_connected (ctx, /* is failed */ 0);
    }
  else
    {
      tls_notify_app_accept (ctx);
    }

  TLS_DBG (1, "Handshake for %u complete. TLS cipher is %s",
	   oc->openssl_ctx_index, SSL_get_cipher (oc->ssl));
  return rv;
}

static inline int
openssl_ctx_write (tls_ctx_t * ctx, stream_session_t * app_session)
{
  openssl_ctx_t *oc = (openssl_ctx_t *) ctx;
  int wrote = 0, rv, read, max_buf = 100 * TLS_CHUNK_SIZE, max_space;
  u32 enq_max, deq_max, deq_now, to_write;
  stream_session_t *tls_session;
  svm_fifo_t *f;

  f = app_session->server_tx_fifo;
  deq_max = svm_fifo_max_dequeue (f);
  if (!deq_max)
    goto check_tls_fifo;

  max_space = max_buf - BIO_ctrl_pending (oc->rbio);
  max_space = (max_space < 0) ? 0 : max_space;
  deq_now = clib_min (deq_max, (u32) max_space);
  to_write = clib_min (svm_fifo_max_read_chunk (f), deq_now);
  wrote = SSL_write (oc->ssl, svm_fifo_head (f), to_write);
  if (wrote <= 0)
    {
      tls_add_vpp_q_evt (app_session->server_tx_fifo, FIFO_EVENT_APP_TX);
      goto check_tls_fifo;
    }
  svm_fifo_dequeue_drop (app_session->server_tx_fifo, wrote);
  if (wrote < deq_now)
    {
      to_write = clib_min (svm_fifo_max_read_chunk (f), deq_now - wrote);
      rv = SSL_write (oc->ssl, svm_fifo_head (f), to_write);
      if (rv > 0)
	{
	  svm_fifo_dequeue_drop (app_session->server_tx_fifo, rv);
	  wrote += rv;
	}
    }

  if (deq_now < deq_max)
    tls_add_vpp_q_evt (app_session->server_tx_fifo, FIFO_EVENT_APP_TX);

check_tls_fifo:

  if (BIO_ctrl_pending (oc->rbio) <= 0)
    return wrote;

  tls_session = session_get_from_handle (ctx->tls_session_handle);
  f = tls_session->server_tx_fifo;
  enq_max = svm_fifo_max_enqueue (f);
  if (!enq_max)
    {
      tls_add_vpp_q_evt (app_session->server_tx_fifo, FIFO_EVENT_APP_TX);
      return wrote;
    }

  deq_now = clib_min (svm_fifo_max_write_chunk (f), enq_max);
  read = BIO_read (oc->rbio, svm_fifo_tail (f), deq_now);
  if (read <= 0)
    {
      tls_add_vpp_q_evt (app_session->server_tx_fifo, FIFO_EVENT_APP_TX);
      return wrote;
    }

  svm_fifo_enqueue_nocopy (f, read);
  tls_add_vpp_q_evt (f, FIFO_EVENT_APP_TX);

  if (read < enq_max && BIO_ctrl_pending (oc->rbio) > 0)
    {
      deq_now = clib_min (svm_fifo_max_write_chunk (f), enq_max - read);
      read = BIO_read (oc->rbio, svm_fifo_tail (f), deq_now);
      if (read > 0)
	svm_fifo_enqueue_nocopy (f, read);
    }

  if (BIO_ctrl_pending (oc->rbio) > 0)
    tls_add_vpp_q_evt (app_session->server_tx_fifo, FIFO_EVENT_APP_TX);

  return wrote;
}

static inline int
openssl_ctx_read (tls_ctx_t * ctx, stream_session_t * tls_session)
{
  int read, wrote = 0, max_space, max_buf = 100 * TLS_CHUNK_SIZE, rv;
  openssl_ctx_t *oc = (openssl_ctx_t *) ctx;
  u32 deq_max, enq_max, deq_now, to_read;
  stream_session_t *app_session;
  svm_fifo_t *f;

  if (PREDICT_FALSE (SSL_in_init (oc->ssl)))
    {
      openssl_ctx_handshake_rx (ctx, tls_session);
      return 0;
    }

  f = tls_session->server_rx_fifo;
  deq_max = svm_fifo_max_dequeue (f);
  max_space = max_buf - BIO_ctrl_pending (oc->wbio);
  max_space = max_space < 0 ? 0 : max_space;
  deq_now = clib_min (deq_max, max_space);
  if (!deq_now)
    goto check_app_fifo;

  to_read = clib_min (svm_fifo_max_read_chunk (f), deq_now);
  wrote = BIO_write (oc->wbio, svm_fifo_head (f), to_read);
  if (wrote <= 0)
    {
      tls_add_vpp_q_evt (tls_session->server_rx_fifo, FIFO_EVENT_BUILTIN_RX);
      goto check_app_fifo;
    }
  svm_fifo_dequeue_drop (f, wrote);
  if (wrote < deq_now)
    {
      to_read = clib_min (svm_fifo_max_read_chunk (f), deq_now - wrote);
      rv = BIO_write (oc->wbio, svm_fifo_head (f), to_read);
      if (rv > 0)
	{
	  svm_fifo_dequeue_drop (f, rv);
	  wrote += rv;
	}
    }
  if (svm_fifo_max_dequeue (f))
    tls_add_vpp_q_evt (tls_session->server_rx_fifo, FIFO_EVENT_BUILTIN_RX);

check_app_fifo:

  if (BIO_ctrl_pending (oc->wbio) <= 0)
    return wrote;

  app_session = session_get_from_handle (ctx->app_session_handle);
  f = app_session->server_rx_fifo;
  enq_max = svm_fifo_max_enqueue (f);
  if (!enq_max)
    {
      tls_add_vpp_q_evt (tls_session->server_rx_fifo, FIFO_EVENT_BUILTIN_RX);
      return wrote;
    }

  deq_now = clib_min (svm_fifo_max_write_chunk (f), enq_max);
  read = SSL_read (oc->ssl, svm_fifo_tail (f), deq_now);
  if (read <= 0)
    {
      tls_add_vpp_q_evt (tls_session->server_rx_fifo, FIFO_EVENT_BUILTIN_RX);
      return wrote;
    }
  svm_fifo_enqueue_nocopy (f, read);
  if (read < enq_max && BIO_ctrl_pending (oc->wbio) > 0)
    {
      deq_now = clib_min (svm_fifo_max_write_chunk (f), enq_max - read);
      read = SSL_read (oc->ssl, svm_fifo_tail (f), deq_now);
      if (read > 0)
	svm_fifo_enqueue_nocopy (f, read);
    }

  tls_notify_app_enqueue (ctx, app_session);
  if (BIO_ctrl_pending (oc->wbio) > 0)
    tls_add_vpp_q_evt (tls_session->server_rx_fifo, FIFO_EVENT_BUILTIN_RX);

  return wrote;
}

static int
openssl_ctx_init_client (tls_ctx_t * ctx)
{
  char *ciphers = "ALL:!ADH:!LOW:!EXP:!MD5:!RC4-SHA:!DES-CBC3-SHA:@STRENGTH";
  long flags = SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3 | SSL_OP_NO_COMPRESSION;
  openssl_ctx_t *oc = (openssl_ctx_t *) ctx;
  openssl_main_t *om = &openssl_main;
  stream_session_t *tls_session;
  const SSL_METHOD *method;
  int rv, err;

  method = SSLv23_client_method ();
  if (method == NULL)
    {
      TLS_DBG (1, "SSLv23_method returned null");
      return -1;
    }

  oc->ssl_ctx = SSL_CTX_new (method);
  if (oc->ssl_ctx == NULL)
    {
      TLS_DBG (1, "SSL_CTX_new returned null");
      return -1;
    }

  SSL_CTX_set_ecdh_auto (oc->ssl_ctx, 1);
  SSL_CTX_set_mode (oc->ssl_ctx, SSL_MODE_ENABLE_PARTIAL_WRITE);
  rv = SSL_CTX_set_cipher_list (oc->ssl_ctx, (const char *) ciphers);
  if (rv != 1)
    {
      TLS_DBG (1, "Couldn't set cipher");
      return -1;
    }

  SSL_CTX_set_options (oc->ssl_ctx, flags);
  SSL_CTX_set_cert_store (oc->ssl_ctx, om->cert_store);

  oc->ssl = SSL_new (oc->ssl_ctx);
  if (oc->ssl == NULL)
    {
      TLS_DBG (1, "Couldn't initialize ssl struct");
      return -1;
    }

  oc->rbio = BIO_new (BIO_s_mem ());
  oc->wbio = BIO_new (BIO_s_mem ());

  BIO_set_mem_eof_return (oc->rbio, -1);
  BIO_set_mem_eof_return (oc->wbio, -1);

  SSL_set_bio (oc->ssl, oc->wbio, oc->rbio);
  SSL_set_connect_state (oc->ssl);

  rv = SSL_set_tlsext_host_name (oc->ssl, ctx->srv_hostname);
  if (rv != 1)
    {
      TLS_DBG (1, "Couldn't set hostname");
      return -1;
    }

  /*
   * 2. Do the first steps in the handshake.
   */
  TLS_DBG (1, "Initiating handshake for [%u]%u", ctx->c_thread_index,
	   oc->openssl_ctx_index);

  tls_session = session_get_from_handle (ctx->tls_session_handle);
  while (1)
    {
      rv = SSL_do_handshake (oc->ssl);
      err = SSL_get_error (oc->ssl, rv);
      openssl_try_handshake_write (oc, tls_session);
      if (err != SSL_ERROR_WANT_WRITE)
	break;
    }

  TLS_DBG (2, "tls state for [%u]%u is su", ctx->c_thread_index,
	   oc->openssl_ctx_index, SSL_state_string_long (oc->ssl));
  return 0;
}

static int
openssl_ctx_init_server (tls_ctx_t * ctx)
{
  char *ciphers = "ALL:!ADH:!LOW:!EXP:!MD5:!RC4-SHA:!DES-CBC3-SHA:@STRENGTH";
  long flags = SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3 | SSL_OP_NO_COMPRESSION;
  openssl_ctx_t *oc = (openssl_ctx_t *) ctx;
  stream_session_t *tls_session;
  const SSL_METHOD *method;
  application_t *app;
  int rv, err;
  BIO *cert_bio;

  app = application_get (ctx->parent_app_index);
  if (!app->tls_cert || !app->tls_key)
    {
      TLS_DBG (1, "tls cert and/or key not configured %d",
	       ctx->parent_app_index);
      return -1;
    }

  method = SSLv23_method ();
  oc->ssl_ctx = SSL_CTX_new (method);
  if (!oc->ssl_ctx)
    {
      clib_warning ("Unable to create SSL context");
      return -1;
    }

  SSL_CTX_set_mode (oc->ssl_ctx, SSL_MODE_ENABLE_PARTIAL_WRITE);
  SSL_CTX_set_options (oc->ssl_ctx, flags);
  SSL_CTX_set_ecdh_auto (oc->ssl_ctx, 1);

  rv = SSL_CTX_set_cipher_list (oc->ssl_ctx, (const char *) ciphers);
  if (rv != 1)
    {
      TLS_DBG (1, "Couldn't set cipher");
      return -1;
    }

  /*
   * Set the key and cert
   */
  cert_bio = BIO_new (BIO_s_mem ());
  BIO_write (cert_bio, app->tls_cert, vec_len (app->tls_cert));
  oc->srvcert = PEM_read_bio_X509 (cert_bio, NULL, NULL, NULL);
  if (!oc->srvcert)
    {
      clib_warning ("unable to parse certificate");
      return -1;
    }
  SSL_CTX_use_certificate (oc->ssl_ctx, oc->srvcert);
  BIO_free (cert_bio);
  cert_bio = BIO_new (BIO_s_mem ());
  BIO_write (cert_bio, app->tls_key, vec_len (app->tls_key));
  oc->pkey = PEM_read_bio_PrivateKey (cert_bio, NULL, NULL, NULL);
  if (!oc->pkey)
    {
      clib_warning ("unable to parse pkey");
      return -1;
    }

  SSL_CTX_use_PrivateKey (oc->ssl_ctx, oc->pkey);
  BIO_free (cert_bio);

  oc->ssl = SSL_new (oc->ssl_ctx);
  if (oc->ssl == NULL)
    {
      TLS_DBG (1, "Couldn't initialize ssl struct");
      return -1;
    }

  oc->rbio = BIO_new (BIO_s_mem ());
  oc->wbio = BIO_new (BIO_s_mem ());

  BIO_set_mem_eof_return (oc->rbio, -1);
  BIO_set_mem_eof_return (oc->wbio, -1);

  SSL_set_bio (oc->ssl, oc->wbio, oc->rbio);
  SSL_set_accept_state (oc->ssl);

  TLS_DBG (1, "Initiating handshake for [%u]%u", ctx->c_thread_index,
	   oc->openssl_ctx_index);

  tls_session = session_get_from_handle (ctx->tls_session_handle);
  while (1)
    {
      rv = SSL_do_handshake (oc->ssl);
      err = SSL_get_error (oc->ssl, rv);
      openssl_try_handshake_write (oc, tls_session);
      if (err != SSL_ERROR_WANT_WRITE)
	break;
    }

  TLS_DBG (2, "tls state for [%u]%u is su", ctx->c_thread_index,
	   oc->openssl_ctx_index, SSL_state_string_long (oc->ssl));
  return 0;
}

static u8
openssl_handshake_is_over (tls_ctx_t * ctx)
{
  openssl_ctx_t *mc = (openssl_ctx_t *) ctx;
  if (!mc->ssl)
    return 0;
  return SSL_is_init_finished (mc->ssl);
}

const static tls_engine_vft_t openssl_engine = {
  .ctx_alloc = openssl_ctx_alloc,
  .ctx_free = openssl_ctx_free,
  .ctx_get = openssl_ctx_get,
  .ctx_get_w_thread = openssl_ctx_get_w_thread,
  .ctx_init_server = openssl_ctx_init_server,
  .ctx_init_client = openssl_ctx_init_client,
  .ctx_write = openssl_ctx_write,
  .ctx_read = openssl_ctx_read,
  .ctx_handshake_is_over = openssl_handshake_is_over,
};

int
tls_init_ca_chain (void)
{
  openssl_main_t *om = &openssl_main;
  tls_main_t *tm = vnet_tls_get_main ();
  BIO *cert_bio;
  X509 *testcert;
  int rv;

  if (access (tm->ca_cert_path, F_OK | R_OK) == -1)
    {
      clib_warning ("Could not initialize TLS CA certificates");
      return -1;
    }

  if (!(om->cert_store = X509_STORE_new ()))
    {
      clib_warning ("failed to create cert store");
      return -1;
    }

  rv = X509_STORE_load_locations (om->cert_store, tm->ca_cert_path, 0);
  if (rv < 0)
    {
      clib_warning ("failed to load ca certificate");
    }

  if (tm->use_test_cert_in_ca)
    {
      cert_bio = BIO_new (BIO_s_mem ());
      BIO_write (cert_bio, test_srv_crt_rsa, test_srv_crt_rsa_len);
      testcert = PEM_read_bio_X509 (cert_bio, NULL, NULL, NULL);
      if (!testcert)
	{
	  clib_warning ("unable to parse certificate");
	  return -1;
	}
      X509_STORE_add_cert (om->cert_store, testcert);
      rv = 0;
    }
  return (rv < 0 ? -1 : 0);
}

static clib_error_t *
tls_openssl_init (vlib_main_t * vm)
{
  vlib_thread_main_t *vtm = vlib_get_thread_main ();
  openssl_main_t *om = &openssl_main;
  clib_error_t *error;
  u32 num_threads;

  num_threads = 1 /* main thread */  + vtm->n_threads;

  if ((error = vlib_call_init_function (vm, tls_init)))
    return error;

  SSL_library_init ();
  SSL_load_error_strings ();

  if (tls_init_ca_chain ())
    {
      clib_warning ("failed to initialize TLS CA chain");
      return 0;
    }

  vec_validate (om->ctx_pool, num_threads - 1);

  tls_register_engine (&openssl_engine, TLS_ENGINE_OPENSSL);
  return 0;
}

VLIB_INIT_FUNCTION (tls_openssl_init);

/* *INDENT-OFF* */
VLIB_PLUGIN_REGISTER () = {
    .version = VPP_BUILD_VER,
    .description = "openssl based TLS Engine",
};
/* *INDENT-ON* */

/*
 * fd.io coding-style-patch-verification: ON
 *
 * Local Variables:
 * eval: (c-set-style "gnu")
 * End:
 */
