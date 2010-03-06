// Copyright (c) 2010 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/browsing_data_appcache_helper.h"

#include "chrome/browser/chrome_thread.h"
#include "chrome/browser/net/chrome_url_request_context.h"
#include "chrome/browser/profile.h"
#include "webkit/appcache/appcache_database.h"
#include "webkit/appcache/appcache_storage.h"

using appcache::AppCacheDatabase;

BrowsingDataAppCacheHelper::BrowsingDataAppCacheHelper(Profile* profile)
    : request_context_getter_(profile->GetRequestContext()),
      is_fetching_(false) {
  DCHECK(request_context_getter_.get());
}

void BrowsingDataAppCacheHelper::StartFetching(Callback0::Type* callback) {
  if (ChromeThread::CurrentlyOn(ChromeThread::UI)) {
    DCHECK(!is_fetching_);
    DCHECK(callback);
    is_fetching_ = true;
    info_collection_ = new appcache::AppCacheInfoCollection;
    completion_callback_.reset(callback);
    ChromeThread::PostTask(ChromeThread::IO, FROM_HERE, NewRunnableMethod(
        this, &BrowsingDataAppCacheHelper::StartFetching, callback));
    return;
  }

  DCHECK(ChromeThread::CurrentlyOn(ChromeThread::IO));
  appcache_info_callback_ =
      new net::CancelableCompletionCallback<BrowsingDataAppCacheHelper>(
          this, &BrowsingDataAppCacheHelper::OnFetchComplete);
  GetAppCacheService()->GetAllAppCacheInfo(info_collection_,
                                           appcache_info_callback_);
}

void BrowsingDataAppCacheHelper::CancelNotification() {
  if (ChromeThread::CurrentlyOn(ChromeThread::UI)) {
    completion_callback_.reset();
    ChromeThread::PostTask(ChromeThread::IO, FROM_HERE, NewRunnableMethod(
        this, &BrowsingDataAppCacheHelper::CancelNotification));
    return;
  }

  if (appcache_info_callback_)
    appcache_info_callback_.release()->Cancel();
}

void BrowsingDataAppCacheHelper::DeleteAppCacheGroup(
    const GURL& manifest_url) {
  if (ChromeThread::CurrentlyOn(ChromeThread::UI)) {
    ChromeThread::PostTask(ChromeThread::IO, FROM_HERE, NewRunnableMethod(
        this, &BrowsingDataAppCacheHelper::DeleteAppCacheGroup,
        manifest_url));
    return;
  }
  GetAppCacheService()->DeleteAppCacheGroup(manifest_url, NULL);
}

void BrowsingDataAppCacheHelper::OnFetchComplete(int rv) {
  if (ChromeThread::CurrentlyOn(ChromeThread::IO)) {
    appcache_info_callback_ = NULL;
    ChromeThread::PostTask(ChromeThread::UI, FROM_HERE, NewRunnableMethod(
        this, &BrowsingDataAppCacheHelper::OnFetchComplete, rv));
    return;
  }

  DCHECK(ChromeThread::CurrentlyOn(ChromeThread::UI));
  DCHECK(is_fetching_);
  is_fetching_ = false;
  if (completion_callback_ != NULL) {
    completion_callback_->Run();
    completion_callback_.reset();
  }
}

ChromeAppCacheService* BrowsingDataAppCacheHelper::GetAppCacheService() {
  DCHECK(ChromeThread::CurrentlyOn(ChromeThread::IO));
  ChromeURLRequestContext* request_context =
      reinterpret_cast<ChromeURLRequestContext*>(
          request_context_getter_->GetURLRequestContext());
  return request_context ? request_context->appcache_service()
                         : NULL;
}
