//
// Copyright (c) 2014 The ANGLE Project Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//

#include "gmock/gmock.h"
#include "gtest/gtest.h"

#include "libANGLE/Fence.h"
#include "libANGLE/renderer/FenceNVImpl.h"
#include "libANGLE/renderer/SyncImpl.h"

using ::testing::_;
using ::testing::Return;
using ::testing::SetArgumentPointee;

namespace
{

//
// FenceNV tests
//

class MockFenceNVImpl : public rx::FenceNVImpl
{
  public:
    virtual ~MockFenceNVImpl() { destroy(); }

    MOCK_METHOD2(set, angle::Result(const gl::Context *, GLenum));
    MOCK_METHOD2(test, angle::Result(const gl::Context *, GLboolean *));
    MOCK_METHOD1(finish, angle::Result(const gl::Context *));

    MOCK_METHOD0(destroy, void());
};

class FenceNVTest : public testing::Test
{
  protected:
    virtual void SetUp()
    {
        mImpl = new MockFenceNVImpl;
        EXPECT_CALL(*mImpl, destroy());
        mFence = new gl::FenceNV(mImpl);
    }

    virtual void TearDown() { delete mFence; }

    MockFenceNVImpl *mImpl;
    gl::FenceNV *mFence;
};

TEST_F(FenceNVTest, DestructionDeletesImpl)
{
    MockFenceNVImpl *impl = new MockFenceNVImpl;
    EXPECT_CALL(*impl, destroy()).Times(1).RetiresOnSaturation();

    gl::FenceNV *fence = new gl::FenceNV(impl);
    delete fence;

    // Only needed because the mock is leaked if bugs are present,
    // which logs an error, but does not cause the test to fail.
    // Ordinarily mocks are verified when destroyed.
    testing::Mock::VerifyAndClear(impl);
}

TEST_F(FenceNVTest, SetAndTestBehavior)
{
    EXPECT_CALL(*mImpl, set(_, _)).WillOnce(Return(angle::Result::Continue)).RetiresOnSaturation();
    EXPECT_FALSE(mFence->isSet());
    EXPECT_EQ(angle::Result::Continue, mFence->set(nullptr, GL_ALL_COMPLETED_NV));
    EXPECT_TRUE(mFence->isSet());
    // Fake the behavior of testing the fence before and after it's passed.
    EXPECT_CALL(*mImpl, test(_, _))
        .WillOnce(DoAll(SetArgumentPointee<1>(static_cast<GLboolean>(GL_FALSE)),
                        Return(angle::Result::Continue)))
        .WillOnce(DoAll(SetArgumentPointee<1>(static_cast<GLboolean>(GL_TRUE)),
                        Return(angle::Result::Continue)))
        .RetiresOnSaturation();
    GLboolean out;
    EXPECT_EQ(angle::Result::Continue, mFence->test(nullptr, &out));
    EXPECT_EQ(GL_FALSE, out);
    EXPECT_EQ(angle::Result::Continue, mFence->test(nullptr, &out));
    EXPECT_EQ(GL_TRUE, out);
}

//
// Sync tests
//

class MockSyncImpl : public rx::SyncImpl
{
  public:
    virtual ~MockSyncImpl() { destroy(); }

    MOCK_METHOD3(set, angle::Result(const gl::Context *, GLenum, GLbitfield));
    MOCK_METHOD4(clientWait, angle::Result(const gl::Context *, GLbitfield, GLuint64, GLenum *));
    MOCK_METHOD3(serverWait, angle::Result(const gl::Context *, GLbitfield, GLuint64));
    MOCK_METHOD2(getStatus, angle::Result(const gl::Context *, GLint *));

    MOCK_METHOD0(destroy, void());
};

class FenceSyncTest : public testing::Test
{
  protected:
    virtual void SetUp()
    {
        mImpl = new MockSyncImpl;
        EXPECT_CALL(*mImpl, destroy());
        mFence = new gl::Sync(mImpl, 1);
        mFence->addRef();
    }

    virtual void TearDown() { mFence->release(nullptr); }

    MockSyncImpl *mImpl;
    gl::Sync *mFence;
};

TEST_F(FenceSyncTest, DestructionDeletesImpl)
{
    MockSyncImpl *impl = new MockSyncImpl;
    EXPECT_CALL(*impl, destroy()).Times(1).RetiresOnSaturation();

    gl::Sync *fence = new gl::Sync(impl, 1);
    fence->addRef();
    fence->release(nullptr);

    // Only needed because the mock is leaked if bugs are present,
    // which logs an error, but does not cause the test to fail.
    // Ordinarily mocks are verified when destroyed.
    testing::Mock::VerifyAndClear(impl);
}

TEST_F(FenceSyncTest, SetAndGetStatusBehavior)
{
    EXPECT_CALL(*mImpl, set(_, _, _))
        .WillOnce(Return(angle::Result::Continue))
        .RetiresOnSaturation();
    EXPECT_EQ(angle::Result::Continue, mFence->set(nullptr, GL_SYNC_GPU_COMMANDS_COMPLETE, 0));
    EXPECT_EQ(static_cast<GLenum>(GL_SYNC_GPU_COMMANDS_COMPLETE), mFence->getCondition());
    // Fake the behavior of testing the fence before and after it's passed.
    EXPECT_CALL(*mImpl, getStatus(_, _))
        .WillOnce(DoAll(SetArgumentPointee<1>(GL_UNSIGNALED), Return(angle::Result::Continue)))
        .WillOnce(DoAll(SetArgumentPointee<1>(GL_SIGNALED), Return(angle::Result::Continue)))
        .RetiresOnSaturation();
    GLint out;
    EXPECT_EQ(angle::Result::Continue, mFence->getStatus(nullptr, &out));
    EXPECT_EQ(GL_UNSIGNALED, out);
    EXPECT_EQ(angle::Result::Continue, mFence->getStatus(nullptr, &out));
    EXPECT_EQ(GL_SIGNALED, out);
}

}  // namespace
