/*
	Copyright (c) 2003, WebThing Ltd
	Author:	Nick Kew <nick@webthing.com>

	Current state: pre-release; some parts work, but none of it
	is suitable for an operational server.  Subject to much change

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.

*/

/*
	Intro and basic documentation available at
	http://www.webthing.com/software/mod_upload/

	Updated July 2003: merged mod_tmpfile into mod_upload
	and wrote the webpage
*/

#include <httpd.h>
#include <apr_strings.h>
#include <util_filter.h>
#include <http_config.h>
#include <http_log.h>


#define BUFLEN 8192
#define KEEPONCLOSE APR_CREATE | APR_READ | APR_WRITE | APR_EXCL

static apr_status_t tmpfile_filter(ap_filter_t *f, apr_bucket_brigade *bbout,
	ap_input_mode_t mode, apr_read_type_e block, apr_off_t nbytes) {

  apr_bucket_brigade* bbin = apr_brigade_create(f->r->pool,
	     f->r->connection->bucket_alloc);

  apr_file_t* tmpfile ;
  char* tmpname = apr_pstrdup(f->r->pool, "/tmp/mod-upload.XXXXXX") ;

  if ( f->ctx ) {
    APR_BRIGADE_INSERT_TAIL(bbout, apr_bucket_eos_create(bbout->bucket_alloc)) ;
    return APR_SUCCESS ;
  }
  if ( apr_file_mktemp(&tmpfile, tmpname, KEEPONCLOSE, f->r->pool) != APR_SUCCESS ) {
	            // error
    ap_remove_input_filter(f) ;
  }
  apr_pool_cleanup_register(f->r->pool, tmpfile,
		(void*)apr_file_close, apr_pool_cleanup_null) ;

  for ( ; ; ) {
    apr_bucket* b ;
    const char* ptr = 0 ;
    apr_size_t bytes ;
#ifdef DEBUG
    ap_log_rerror(APLOG_MARK,APLOG_DEBUG,0, f->r, "get_brigade") ;
#endif
    ap_get_brigade(f->next, bbin, AP_MODE_READBYTES, APR_BLOCK_READ, BUFLEN) ;
    for ( b = APR_BRIGADE_FIRST(bbin) ;
	b != APR_BRIGADE_SENTINEL(bbin) && ! f->ctx ;
	b = APR_BUCKET_NEXT(b) ) {
      if ( APR_BUCKET_IS_EOS(b) ) {
	f->ctx = f ;	// just using it as a flag; any nonzero will do
	apr_file_flush(tmpfile) ;
	apr_brigade_puts(bbout, ap_filter_flush, f, tmpname) ;
	APR_BRIGADE_INSERT_TAIL(bbout,
		apr_bucket_eos_create(bbout->bucket_alloc) ) ;
      } else if ( apr_bucket_read(b, &ptr, &bytes, APR_BLOCK_READ)
		== APR_SUCCESS ) {
#ifdef DEBUG
  ap_log_rerror(APLOG_MARK,APLOG_DEBUG,0, f->r, "	%d bytes in bucket", bytes) ;
#endif
	apr_file_write(tmpfile, ptr, &bytes) ;
      }
    }
    if ( f->ctx )
      break ;
    else
      apr_brigade_cleanup(bbin) ;
  }

  apr_brigade_destroy(bbin) ;

  return APR_SUCCESS ;
}


module AP_MODULE_DECLARE_DATA upload_module ;

typedef struct {
  char* file_field ;
  int form_size ;
} upload_conf ;

static void* upload_create_dir_config(apr_pool_t* p, char* x) {
  upload_conf* conf = apr_pcalloc(p, sizeof(upload_conf)) ;
  conf->form_size = 8 ;
  return conf ;
}
static const char* set_filename(cmd_parms* cmd, void* cfg, const char* name) {
  upload_conf* conf = (upload_conf*) cfg ;
  conf->file_field = apr_pstrdup(cmd->pool, name) ;
  return NULL ;
}
static const char* set_formsize(cmd_parms* cmd, void* cfg, const char* sz) {
  upload_conf* conf = (upload_conf*) cfg ;
  conf->form_size = atoi(sz) ;
  return NULL ;
}

static char* lccopy(apr_pool_t* p, const char* str) {
  char* ret = apr_pstrdup(p, str) ;
  char* q ;
  for ( q = ret ; *q ; ++q )
    if ( isupper(*q) )
      *q = tolower(*q) ;
  return ret ;
}

static char* get_boundary(apr_pool_t* p, const char* ctype) {
  char* ret = NULL ;
  if ( ctype ) {
    char* lctype = lccopy(p, ctype) ;
    char* bdy = strstr(lctype, "boundary") ;
    if ( bdy ) {
      char* ptr = strchr(bdy, '=') ;
      if ( ptr ) {
	bdy = (char*) ctype + ( ptr - lctype ) + 1 ;
	for ( ptr = bdy; *ptr; ++ptr )
	  if ( *ptr == ';' || isspace(*ptr) )
	    *ptr = 0 ;
	ret = apr_pstrdup(p, bdy) ;
      }
    }
  }
  return ret ;
}

typedef struct upload_ctx {
  apr_pool_t* pool ;
  apr_table_t* form ;
//  apr_bucket_brigade* bbin ;
  char* boundary ;
  char* key ;
  char* val ;
  char* file_field ;
  char* leftover ;
  enum { p_none, p_head, p_field, p_end, p_done } parse_state ;
  char is_file ;
} upload_ctx ;

static apr_status_t upload_filter_init(ap_filter_t* f) {
  upload_conf* conf = ap_get_module_config(f->r->per_dir_config,&upload_module);
  upload_ctx* ctx = apr_palloc(f->r->pool, sizeof(upload_ctx)) ;
  // check content-type, get boundary or error
  const char* ctype = apr_table_get(f->r->headers_in, "Content-Type") ;

  if ( ! ctype || ! conf->file_field ||
	strncmp ( ctype , "multipart/form-data", 19 ) ) {
    ap_remove_input_filter(f) ;
    return APR_SUCCESS ;
  }

  ctx->pool = f->r->pool ;
  ctx->form = apr_table_make(ctx->pool, conf->form_size) ;
  ctx->boundary = get_boundary(f->r->pool, ctype) ;
  ctx->parse_state = p_none ;
  ctx->key = ctx->val = 0 ;
  ctx->file_field = conf->file_field ;
  ctx->is_file = 0 ;
  ctx->leftover = 0 ;
//  ctx->bbin = apr_brigade_create(f->r->pool, f->r->connection->bucket_alloc) ;

  /* save the table in request_config */
  ap_set_module_config(f->r->request_config, &upload_module, ctx->form) ;

  f->ctx = ctx ;
  return APR_SUCCESS ;
}

static void set_header(upload_ctx* ctx, const char* data) {
  char* colon = strchr(data, ':' ) ;
  if ( colon ) {
    *colon++ = 0 ;
    while ( isspace(*colon) )
      ++colon ;
    if ( ! strcasecmp( data, "Content-Disposition" ) ) {
      char* np = strstr(colon, "name=") ;
      if ( np )
	np = strstr(colon, "name=\"") ;
      if ( np ) {
	char* ep ;
	np += 6 ;
	ep = strchr(np, '"') ;
	if ( ep ) {
	  *ep = 0 ;
	  ctx->key = apr_pstrndup(ctx->pool, np, ep-np) ;
	  if ( ! strcasecmp(ctx->key, ctx->file_field) )
	    ctx->is_file = 1 ;
	}
      }
    }
  }
}
static void set_body(upload_ctx* ctx, const char* data) {
  const char* cr = strchr(data, '\r') ;
  char* tmp = apr_pstrndup(ctx->pool, data, cr-data) ;
  if ( ctx->val )
    ctx->val = apr_pstrcat(ctx->pool, ctx->val, tmp, NULL) ;
  else
    ctx->val = tmp ;
}
static enum { boundary_part, boundary_end, boundary_none }
	is_boundary( upload_ctx* ctx, const char* p) {
  size_t blen = strlen(ctx->boundary) ;
  if ( strlen(p) < 2 + blen )
	return boundary_none ;
  if ( ( p[0] != '-' ) || ( p[1] != '-' ) )
	return boundary_none ;
  if ( strncmp(ctx->boundary, p+2,  blen ) )
	return boundary_none ;
  if ( ( p[blen+2] == '-' ) && ( p[blen+3] == '-' ) )
	return boundary_end ;
  else
	return boundary_part ;
}
static void end_body(upload_ctx* ctx) {
  if ( ! ctx->is_file )
    apr_table_set(ctx->form, ctx->key, ctx->val) ;
  else
    ctx->is_file = 0 ;

  ctx->key = ctx->val = 0 ;
}
static apr_status_t upload_filter(ap_filter_t *f, apr_bucket_brigade *bbout,
	ap_input_mode_t mode, apr_read_type_e block, apr_off_t nbytes) {

  char* buf = 0 ;
  char* p = buf ;
  char* e ;
 
  int ret = APR_SUCCESS ;
 
  apr_size_t bytes = 0 ;
  apr_bucket* b ;
  apr_bucket_brigade* bbin ;
 
  upload_ctx* ctx = (upload_ctx*) f->ctx ;
  if ( ctx->parse_state == p_done ) {
    // send an EOS
    APR_BRIGADE_INSERT_TAIL(bbout, apr_bucket_eos_create(bbout->bucket_alloc) ) ;
    return APR_SUCCESS ;
  }

  /* should be more efficient to do this in-place without resorting
   * to a new brigade
   */
  bbin = apr_brigade_create(f->r->pool, f->r->connection->bucket_alloc) ;

  if ( ret = ap_get_brigade(f->next, bbin, mode, block, nbytes) ,
	ret != APR_SUCCESS )
     return ret ;


  for ( b = APR_BRIGADE_FIRST(bbin) ;
	b != APR_BRIGADE_SENTINEL(bbin) ;
	b = APR_BUCKET_NEXT(b) ) {
    const char* ptr = buf ;
    if ( APR_BUCKET_IS_EOS(b) ) {
      ctx->parse_state = p_done ;
      APR_BRIGADE_INSERT_TAIL(bbout,
	 apr_bucket_eos_create(bbout->bucket_alloc) ) ;
      apr_brigade_destroy(bbin) ;
      return APR_SUCCESS ;
    } else if ( apr_bucket_read(b, &ptr, &bytes, APR_BLOCK_READ)
		== APR_SUCCESS ) {
      const char* p = ptr ;
      while ( e = strchr(p, '\n'), ( e && ( e < (ptr+bytes) ) ) ) {
	const char* ptmp = p ;
	*e = 0 ;
	if ( ctx->leftover ) {
		// this'll be grossly inefficient if we get lots of
		// little buckets (we don't in my setup:-)
	  ptmp = apr_pstrcat(f->r->pool, ctx->leftover, p, NULL) ;
	  ctx->leftover = 0 ;
	}
	switch ( ctx->parse_state ) {
	  case p_none:
	    if ( is_boundary(ctx, ptmp) == boundary_part )
	      ctx->parse_state = p_head ;
	    break ;
	  case p_head:
	    if ( (! *ptmp) || ( *ptmp == '\r') )
	      ctx->parse_state = p_field ;
	    else
	      set_header(ctx, ptmp) ;
	    break ;
	  case p_field:
	    switch ( is_boundary(ctx, ptmp) ) {
	      case boundary_part:
		end_body(ctx) ;
		ctx->parse_state = p_head ;
		break ;
	      case boundary_end:
		end_body(ctx) ;
		ctx->parse_state = p_end ;
		break ;
	      case boundary_none:
		if ( ctx->is_file ) {
		  apr_brigade_puts(bbout, ap_filter_flush, f, ptmp) ;
		  apr_brigade_putc(bbout, ap_filter_flush, f, '\n') ;
		} else
		  set_body(ctx, ptmp) ;
		break ;
	    }
	    break ;
	  case p_end:
	    //APR_BRIGADE_INSERT_TAIL(bbout,
	//	apr_bucket_eos_create(bbout->bucket_alloc) ) ;
	    ctx->parse_state = p_done ;
	  case p_done:
	    break ;
	}
	if ( e - ptr >= bytes )
	  break ;
	p = e + 1 ;
      }
      if ( ( ctx->parse_state != p_end ) && ( ctx->parse_state != p_done ) ) {
	size_t bleft = bytes - (p-ptr) ;
	ctx->leftover = apr_pstrndup(f->r->pool, p, bleft ) ;
#ifdef DEBUG
  ap_log_rerror(APLOG_MARK,APLOG_DEBUG,0, f->r, "leftover %d bytes\n\t%s\n\t%s\n", bleft, ctx->leftover, p) ;
#endif
      }
    }
  }
  apr_brigade_destroy(bbin) ;
  return ret ;
}


static const command_rec upload_cmds[] = {
	AP_INIT_TAKE1("UploadField", set_filename, NULL, OR_ALL,
		"Set name of file upload field" ) ,
	AP_INIT_TAKE1("UploadFormSize", set_formsize, NULL, OR_ALL,
		"Set number of form fields" ) ,
	{NULL}
} ;

static void upload_hooks(apr_pool_t* p) {

  ap_register_input_filter("tmpfile-filter", tmpfile_filter,
		NULL, AP_FTYPE_RESOURCE) ;
  ap_register_input_filter("upload-filter", upload_filter,
		upload_filter_init, AP_FTYPE_RESOURCE) ;

} ;

module AP_MODULE_DECLARE_DATA upload_module = {
	STANDARD20_MODULE_STUFF,
	upload_create_dir_config,
	NULL,
	NULL,
	NULL,
	upload_cmds,
	upload_hooks
} ;

/* Export the form data we've parsed */
apr_table_t* mod_upload_form(request_rec* r) {
  return (apr_table_t*)
	ap_get_module_config(r->request_config,&upload_module);
}
