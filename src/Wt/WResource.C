/*
 * Copyright (C) 2008 Emweb bvba, Kessel-Lo, Belgium.
 *
 * See the LICENSE file for terms of use.
 */
#include <string>

#include "Wt/WResource"
#include "Wt/WApplication"
#include "Wt/Http/Request"
#include "Wt/Http/Response"

#include "WebController.h"
#include "WebRequest.h"
#include "WebSession.h"
#include "WebUtils.h"

#ifdef WT_THREADED
#include <boost/thread/recursive_mutex.hpp>
#endif // WT_THREADED

namespace Wt {

LOGGER("WResource");

/*
 * Resource locking strategy:
 *
 * A resource is reentrant: multiple calls to handleRequest can happen
 * simultaneously.
 *
 * The mutex_ protects:
 *  - beingDeleted_: indicates that the resource wants to be deleted,
 *    and thus should no longer be used (by continuations)
 *  - useCount_: number of requests currently being handled
 *  - continuations_: the list of continuations
 */

WResource::UseLock::UseLock()
  : resource_(0)
{ }

bool WResource::UseLock::use(WResource *resource)
{
#ifdef WT_THREADED
  if (resource && !resource->beingDeleted_) {
    resource_ = resource;
    ++resource_->useCount_;

    return true;
  } else
    return false;
#else
  return true;
#endif
}

WResource::UseLock::~UseLock()
{
#ifdef WT_THREADED
  if (resource_) {
    boost::recursive_mutex::scoped_lock lock(*resource_->mutex_);
    --resource_->useCount_;
    if (resource_->useCount_ == 0)
      resource_->useDone_.notify_one();
  }
#endif
}

WResource::WResource(WObject* parent)
  : WObject(parent),
    dataChanged_(this),
    trackUploadProgress_(false),
    dispositionType_(NoDisposition)
{ 
#ifdef WT_THREADED
  mutex_.reset(new boost::recursive_mutex());
  beingDeleted_ = false;
  useCount_ = 0;
#endif // WT_THREADED
}

void WResource::beingDeleted()
{
  std::vector<Http::ResponseContinuationPtr> cs;

  LOG_DEBUG("beingDeleted()");

  {
#ifdef WT_THREADED
    boost::recursive_mutex::scoped_lock lock(*mutex_);
    beingDeleted_ = true;

    while (useCount_ > 0)
      useDone_.wait(lock);
#endif // WT_THREADED

    cs = continuations_;

    continuations_.clear();
  }

  for (unsigned i = 0; i < cs.size(); ++i)
    cs[i]->cancel(true);
}

WResource::~WResource()
{
  beingDeleted();

  WApplication *app = WApplication::instance();
  if (app) {
    app->removeExposedResource(this);
    if (trackUploadProgress_)
      WebSession::instance()->controller()->removeUploadProgressUrl(url());
  }
}

void WResource::setUploadProgress(bool enabled)
{
  if (trackUploadProgress_ != enabled) {
    trackUploadProgress_ = enabled;

    WebController *c = WebSession::instance()->controller();
    if (enabled)
      c->addUploadProgressUrl(url());
    else
      c->removeUploadProgressUrl(url());
  }
}

void WResource::haveMoreData()
{
  std::vector<Http::ResponseContinuationPtr> cs;

  {
#ifdef WT_THREADED
    boost::recursive_mutex::scoped_lock lock(*mutex_);
#endif // WT_THREADED
    cs = continuations_;
  }

  for (unsigned i = 0; i < cs.size(); ++i)
    cs[i]->haveMoreData();
}

void WResource::doContinue(Http::ResponseContinuationPtr continuation)
{
  WebResponse *webResponse = continuation->response();
  WebRequest *webRequest = webResponse;

  try {
    handle(webRequest, webResponse, continuation);
  } catch (std::exception& e) {
    LOG_ERROR("exception while handling resource continuation: " << e.what());
  } catch (...) {
    LOG_ERROR("exception while handling resource continuation");
  }
}

void WResource::removeContinuation(Http::ResponseContinuationPtr continuation)
{
#ifdef WT_THREADED
  boost::recursive_mutex::scoped_lock lock(*mutex_);
#endif
  Utils::erase(continuations_, continuation);
}

Http::ResponseContinuationPtr
WResource::addContinuation(Http::ResponseContinuation *c)
{
  Http::ResponseContinuationPtr result(c);

#ifdef WT_THREADED
  boost::recursive_mutex::scoped_lock lock(*mutex_);
#endif
  continuations_.push_back(result);

  return result;
}

void WResource::handle(WebRequest *webRequest, WebResponse *webResponse,
		       Http::ResponseContinuationPtr continuation)
{
  /*
   * If we are a new request for a dynamic resource, then we will have
   * the session lock at this point and thus the resource is protected
   * against deletion.
   *
   * If we come from a continuation, then the continuation increased the
   * use count and we are thus protected against deletion.
   */
  WebSession::Handler *handler = WebSession::Handler::instance();
  UseLock useLock;

  if (handler && !continuation) {
#ifdef WT_THREADED
    boost::recursive_mutex::scoped_lock lock(*mutex_);

    if (!useLock.use(this))
      return;

    if (handler->haveLock() && 
	handler->lockOwner() == boost::this_thread::get_id()) {
      handler->lock().unlock();
    }
#endif // WT_THREADED
  }

  Http::Request request(*webRequest, continuation.get());
  Http::Response response(this, webResponse, continuation);

  if (!continuation)
    response.setStatus(200);

  handleRequest(request, response);

  if (!response.continuation_ || !response.continuation_->resource_) {
    if (response.continuation_)
      removeContinuation(response.continuation_);

    response.out(); // trigger committing the headers if still necessary

    webResponse->flush(WebResponse::ResponseDone);
  } else {
    webResponse->flush
      (WebResponse::ResponseFlush,
       boost::bind(&Http::ResponseContinuation::readyToContinue,
		   response.continuation_, _1));
  }
}

void WResource::handleAbort(const Http::Request& request)
{ }

void WResource::suggestFileName(const WString& name,
                                DispositionType dispositionType)
{
  suggestedFileName_ = name;
  dispositionType_ = dispositionType;

  currentUrl_.clear();
}

void WResource::setInternalPath(const std::string& path)
{
  internalPath_ = path;

  currentUrl_.clear();
}

void WResource::setDispositionType(DispositionType dispositionType)
{
  dispositionType_ = dispositionType;
}

void WResource::setChanged()
{
  generateUrl();

  dataChanged_.emit();
}

const std::string& WResource::url() const
{
  if (currentUrl_.empty())
    (const_cast<WResource *>(this))->generateUrl();

  return currentUrl_;
}

const std::string& WResource::generateUrl()
{
  WApplication *app = WApplication::instance();

  if (app) {
    WebController *c = 0;
    if (trackUploadProgress_)
      c = WebSession::instance()->controller();

    if (c && !currentUrl_.empty())
      c->removeUploadProgressUrl(currentUrl_);
    currentUrl_ = app->addExposedResource(this);
    if (c)
      c->addUploadProgressUrl(currentUrl_);    
  } else
    currentUrl_ = internalPath_;

  return currentUrl_;
}

void WResource::write(WT_BOSTREAM& out,
		      const Http::ParameterMap& parameters,
		      const Http::UploadedFileMap& files)
{
  Http::Request  request(parameters, files);
  Http::Response response(this, out);

  handleRequest(request, response);

  // While the resource indicates more data to be sent, get it too.
  while (response.continuation_	&& response.continuation_->resource_) {
    response.continuation_->resource_ = 0;
    request.continuation_ = response.continuation_.get();

    handleRequest(request, response);
  }
}

}
