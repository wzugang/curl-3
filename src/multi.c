#include "curl-common.h"
#include <time.h>

#if LIBCURL_VERSION_MAJOR > 7 || (LIBCURL_VERSION_MAJOR == 7 && LIBCURL_VERSION_MINOR >= 28)
#define HAS_MULTI_WAIT 1
#endif

CURLM *global_multi = NULL;
int global_pending = 0;

/* Important:
 * The ref->busy field means the handle is used by global_multi system
 * We need this because there is no way to query the multi handle for pending handles
 * The ref->locked is used to lock the handle for any use.
 */

SEXP R_multi_remove(SEXP handle_ptr){
  reference *ref = get_ref(handle_ptr);
  if(ref->busy){
    massert(curl_multi_remove_handle(global_multi, ref->handle));
    ref->busy = 0;
    ref->locked = 0;
    ref->refCount--;
    clean_handle(ref);
    global_pending--;
  }
  return handle_ptr;
}

SEXP R_multi_add(SEXP handle_ptr, SEXP complete, SEXP error){
  reference *ref = get_ref(handle_ptr);
  if(ref->locked)
    Rf_errorcall(R_NilValue, "Handle is locked. Probably in use in a connection or async request.");

  /* buffer body */
  //memory body = {NULL, 0};
  //curl_easy_setopt(ref->handle, CURLOPT_WRITEFUNCTION, append_buffer);
  //curl_easy_setopt(ref->handle, CURLOPT_WRITEDATA, &body);

  /* add to scheduler */
  massert(curl_multi_add_handle(global_multi, ref->handle));
  global_pending++;
  ref->refCount++;
  ref->locked = 1;
  ref->busy = 1;
  return handle_ptr;
}


SEXP R_multi_run(SEXP multiplex, SEXP connections, SEXP timeout){

  #ifdef CURLPIPE_MULTIPLEX
    if(asLogical(multiplex))
      massert(curl_multi_setopt(global_multi, CURLMOPT_PIPELINING, CURLPIPE_MULTIPLEX));
  #endif
  massert(curl_multi_setopt(global_multi, CURLMOPT_MAX_TOTAL_CONNECTIONS, asInteger(connections)));

  int still_running = 1;
  int total_success = 0;
  int total_fail = 0;
  double time_max = asReal(timeout);

  clock_t time_start = clock();
  double seconds_elapsed = 0;
  while(still_running) {
    if(pending_interrupt())
      break;
    /* Required by old versions of libcurl */
    CURLMcode res = CURLM_CALL_MULTI_PERFORM;
    while(res == CURLM_CALL_MULTI_PERFORM)
      res = curl_multi_perform(global_multi, &(still_running));

    /* check for multi errors */
    if(res != CURLM_OK)
      break;

    /* check for completed requests */
    int msgq = 0;
    do {
      CURLMsg *m = curl_multi_info_read(global_multi, &msgq);
      if(m && (m->msg == CURLMSG_DONE)){
        reference *ref = NULL;
        CURL *handle = m->easy_handle;
        assert(curl_easy_getinfo(handle, CURLINFO_PRIVATE, &ref));

        // release the handle so that it can be reused in callback
        massert(curl_multi_remove_handle(global_multi, handle));
        global_pending--;
        ref->busy = 0;
        ref->locked = 0;

        // execute the success or error callback
        // callbacks must be trycatch! we should continue the loop
        CURLcode status = m->data.result;
        if(status == CURLE_OK){
          total_success++;
          Rprintf("Success!\n"); //placeholder for success callback
        } else {
          total_fail++;
          Rprintf("Error: %s\n", curl_easy_strerror(status)); //placeholder for error callback
        }

        // collect garbage after executing callbacks
        ref->refCount--;
        clean_handle(ref);
      }
      /* check for timeout */
      seconds_elapsed = (double) (clock() - time_start) / CLOCKS_PER_SEC;
      Rprintf("elapsed: %f, max: %f\n", seconds_elapsed, time_max);
    } while (msgq > 0 && seconds_elapsed < time_max);
  }
  SEXP res = PROTECT(Rf_list3(
    ScalarInteger(total_success),
    ScalarInteger(total_fail),
    ScalarInteger(global_pending)
  ));
  SEXP names = PROTECT(allocVector(STRSXP, 3));
  SET_STRING_ELT(names, 0, mkChar("success"));
  SET_STRING_ELT(names, 1, mkChar("error"));
  SET_STRING_ELT(names, 2, mkChar("pending"));
  setAttrib(res, R_NamesSymbol, names);
  UNPROTECT(2);
  return res;
}
