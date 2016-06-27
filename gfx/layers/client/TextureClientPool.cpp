/* -*- Mode: C++; tab-width: 20; indent-tabs-mode: nil; c-basic-offset: 2 -*-
* This Source Code Form is subject to the terms of the Mozilla Public
* License, v. 2.0. If a copy of the MPL was not distributed with this
* file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "TextureClientPool.h"
#include "CompositableClient.h"
#include "mozilla/layers/CompositableForwarder.h"
#include "mozilla/layers/TextureForwarder.h"
#include "mozilla/layers/TiledContentClient.h"

#include "gfxPrefs.h"

#include "nsComponentManagerUtils.h"

#define TCP_LOG(...)
//#define TCP_LOG(...) printf_stderr(__VA_ARGS__);

namespace mozilla {
namespace layers {


TextureClientPool::TextureClientPool(LayersBackend aLayersBackend,
                                     gfx::SurfaceFormat aFormat,
                                     gfx::IntSize aSize,
                                     TextureFlags aFlags,
                                     uint32_t aInitialUnusedTextureClients,
                                     uint32_t aShrinkTimeoutMsec,
                                     TextureForwarder* aAllocator)
  : mBackend(aLayersBackend)
  , mFormat(aFormat)
  , mSize(aSize)
  , mFlags(aFlags)
  , mInitialUnusedTextureClients(aInitialUnusedTextureClients)
  , mShrinkTimeoutMsec(aShrinkTimeoutMsec)
  , mOutstandingClients(0)
  , mSurfaceAllocator(aAllocator)
{
  TCP_LOG("TexturePool %p created with maximum unused texture clients %u and timeout %u\n",
      this, mInitialUnusedTextureClients, aShrinkTimeoutMsec);
  mTimer = do_CreateInstance("@mozilla.org/timer;1");
  if (aFormat == gfx::SurfaceFormat::UNKNOWN) {
    gfxWarning() << "Creating texture pool for SurfaceFormat::UNKNOWN format";
  }
}

TextureClientPool::~TextureClientPool()
{
  mTimer->Cancel();
}

#ifdef GFX_DEBUG_TRACK_CLIENTS_IN_POOL
static bool TestClientPool(const char* what,
                           TextureClient* aClient,
                           TextureClientPool* aPool)
{
  if (!aClient || !aPool) {
    return false;
  }

  TextureClientPool* actual = aClient->mPoolTracker;
  bool ok = (actual == aPool);
  if (ok) {
    ok = (aClient->GetFormat() == aPool->GetFormat());
  }

  if (!ok) {
    if (actual) {
      gfxCriticalError() << "Pool error(" << what << "): "
                   << aPool << "-" << aPool->GetFormat() << ", "
                   << actual << "-" << actual->GetFormat() << ", "
                   << aClient->GetFormat();
      MOZ_CRASH("GFX: Crashing with actual");
    } else {
      gfxCriticalError() << "Pool error(" << what << "): "
                   << aPool << "-" << aPool->GetFormat() << ", nullptr, "
                   << aClient->GetFormat();
      MOZ_CRASH("GFX: Crashing without actual");
    }
  }
  return ok;
}
#endif

already_AddRefed<TextureClient>
TextureClientPool::GetTextureClient()
{
  // Try to fetch a client from the pool
  RefPtr<TextureClient> textureClient;

  // We initially allocate mInitialUnusedTextureClients for our pool. If we run
  // out of TextureClients, we allocate additional TextureClients one-by-one
  // and try to reduce the size of the pool back down to
  // mInitialUnusedTextureClients
  if (!mTextureClients.size()) {
    size_t size = mOutstandingClients ? 1 : mInitialUnusedTextureClients;
    AllocateTextureClients(size);
  }

  mOutstandingClients++;
  textureClient = mTextureClients.top();
  mTextureClients.pop();
#ifdef GFX_DEBUG_TRACK_CLIENTS_IN_POOL
  if (textureClient) {
    textureClient->mPoolTracker = this;
  }
  DebugOnly<bool> ok = TestClientPool("fetch", textureClient, this);
  MOZ_ASSERT(ok);
#endif
  TCP_LOG("TexturePool %p giving %p from pool; size %u outstanding %u\n",
      this, textureClient.get(), mTextureClients.size(), mOutstandingClients);

  return textureClient.forget();
}

void
TextureClientPool::AllocateTextureClients(size_t aSize)
{
  TCP_LOG("TexturePool %p allocating %u clients, outstanding %u\n",
      this, aSize, mOutstandingClients);

  while (mTextureClients.size() < aSize) {
    if (gfxPrefs::ForceShmemTiles()) {
      // gfx::BackendType::NONE means use the content backend
      mTextureClients.push(TextureClient::CreateForRawBufferAccess(mSurfaceAllocator,
        mFormat, mSize, gfx::BackendType::NONE,
        mFlags, ALLOC_DEFAULT));
    } else {
      mTextureClients.push(TextureClient::CreateForDrawing(mSurfaceAllocator,
        mFormat, mSize, mBackend, BackendSelector::Content, mFlags));
    }
  }
}

void
TextureClientPool::ReturnTextureClient(TextureClient *aClient)
{
  if (!aClient) {
    return;
  }
#ifdef GFX_DEBUG_TRACK_CLIENTS_IN_POOL
  DebugOnly<bool> ok = TestClientPool("return", aClient, this);
  MOZ_ASSERT(ok);
#endif
  // Add the client to the pool:
  MOZ_ASSERT(mOutstandingClients > mTextureClientsDeferred.size());
  mOutstandingClients--;
  mTextureClients.push(aClient);
  TCP_LOG("TexturePool %p had client %p returned; size %u outstanding %u\n",
      this, aClient, mTextureClients.size(), mOutstandingClients);

  // Shrink down if we're beyond our maximum size
  ShrinkToMaximumSize();
}

void
TextureClientPool::ReturnTextureClientDeferred(TextureClient* aClient)
{
  if (!aClient) {
    return;
  }
  MOZ_ASSERT(aClient->GetReadLock());
#ifdef GFX_DEBUG_TRACK_CLIENTS_IN_POOL
  DebugOnly<bool> ok = TestClientPool("defer", aClient, this);
  MOZ_ASSERT(ok);
#endif
  mTextureClientsDeferred.push_back(aClient);
  TCP_LOG("TexturePool %p had client %p defer-returned, size %u outstanding %u\n",
      this, aClient, mTextureClientsDeferred.size(), mOutstandingClients);
  ShrinkToMaximumSize();
}

void
TextureClientPool::ShrinkToMaximumSize()
{
  TCP_LOG("TexturePool %p shrinking to max size %u; total outstanding %u\n",
      this, mInitialUnusedTextureClients, mOutstandingClients);

  // We're over our desired maximum size, immediately shrink down to the
  // maximum.
  //
  // We cull from the deferred TextureClients first, as we can't reuse those
  // until they get returned.
  uint32_t totalUnusedTextureClients = mTextureClients.size() + mTextureClientsDeferred.size();

  while (totalUnusedTextureClients > mInitialUnusedTextureClients) {
    if (mTextureClientsDeferred.size()) {
      mOutstandingClients--;
      TCP_LOG("TexturePool %p dropped deferred client %p; %u remaining\n",
          this, mTextureClientsDeferred.front().get(),
          mTextureClientsDeferred.size() - 1);
      mTextureClientsDeferred.pop_front();
    } else {
      TCP_LOG("TexturePool %p dropped non-deferred client %p; %u remaining\n",
          this, mTextureClients.top().get(), mTextureClients.size() - 1);
      mTextureClients.pop();
    }
    totalUnusedTextureClients--;
  }
}

void
TextureClientPool::ReturnDeferredClients()
{
  TCP_LOG("TexturePool %p returning %u deferred clients to pool\n",
      this, mTextureClientsDeferred.size());

  if (mTextureClientsDeferred.empty()) {
    return;
  }

  ReturnUnlockedClients();
  ShrinkToMaximumSize();
}

void
TextureClientPool::ReturnUnlockedClients()
{
  for (auto it = mTextureClientsDeferred.begin(); it != mTextureClientsDeferred.end();) {
    MOZ_ASSERT((*it)->GetReadLock()->GetReadCount() >= 1);
    // Last count is held by the lock itself.
    if (!(*it)->IsReadLocked()) {
      mTextureClients.push(*it);
      it = mTextureClientsDeferred.erase(it);

      MOZ_ASSERT(mOutstandingClients > 0);
      mOutstandingClients--;
    } else {
      it++;
    }
  }
}

void
TextureClientPool::ReportClientLost()
{
  MOZ_ASSERT(mOutstandingClients > mTextureClientsDeferred.size());
  mOutstandingClients--;
  TCP_LOG("TexturePool %p getting report client lost; down to %u outstanding\n",
      this, mOutstandingClients);
}

void
TextureClientPool::Clear()
{
  TCP_LOG("TexturePool %p getting cleared\n", this);
  while (!mTextureClients.empty()) {
    TCP_LOG("TexturePool %p releasing client %p\n",
        this, mTextureClients.top().get());
    mTextureClients.pop();
  }
  while (!mTextureClientsDeferred.empty()) {
    MOZ_ASSERT(mOutstandingClients > 0);
    mOutstandingClients--;
    TCP_LOG("TexturePool %p releasing deferred client %p\n",
        this, mTextureClientsDeferred.front().get());
    mTextureClientsDeferred.pop_front();
  }
}

void TextureClientPool::Destroy()
{
  Clear();
  mInitialUnusedTextureClients = 0;
}

} // namespace layers
} // namespace mozilla
