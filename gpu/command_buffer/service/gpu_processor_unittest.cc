// Copyright (c) 2009 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/at_exit.h"
#include "base/message_loop.h"
#include "gpu/command_buffer/common/command_buffer_mock.h"
#include "gpu/command_buffer/service/mocks.h"
#include "gpu/command_buffer/service/gpu_processor.h"
#include "gpu/command_buffer/service/gles2_cmd_decoder.h"
#include "gpu/command_buffer/service/gles2_cmd_decoder_mock.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "testing/gmock/include/gmock/gmock.h"

using testing::_;
using testing::DoAll;
using testing::Invoke;
using testing::NiceMock;
using testing::Return;
using testing::SetArgumentPointee;
using testing::StrictMock;

namespace gpu {

const size_t kRingBufferSize = 1024;
const size_t kRingBufferEntries = kRingBufferSize / sizeof(CommandBufferEntry);

class GPUProcessorTest : public testing::Test {
 protected:
  virtual void SetUp() {
    shared_memory_.reset(new ::base::SharedMemory);
    shared_memory_->Create(std::wstring(), false, false, kRingBufferSize);
    shared_memory_->Map(kRingBufferSize);
    buffer_ = static_cast<int32*>(shared_memory_->memory());
    shared_memory_buffer_.ptr = buffer_;
    shared_memory_buffer_.size = kRingBufferSize;
    memset(buffer_, 0, kRingBufferSize);

    command_buffer_.reset(new MockCommandBuffer);
    ON_CALL(*command_buffer_.get(), GetRingBuffer())
      .WillByDefault(Return(shared_memory_buffer_));
    ON_CALL(*command_buffer_.get(), GetSize())
      .WillByDefault(Return(kRingBufferEntries));

    async_api_.reset(new StrictMock<AsyncAPIMock>);

    decoder_ = new gles2::MockGLES2Decoder();

    parser_ = new CommandParser(buffer_,
                                kRingBufferEntries,
                                0,
                                kRingBufferEntries,
                                0,
                                async_api_.get());

    processor_ = new GPUProcessor(command_buffer_.get(),
                                  decoder_,
                                  parser_,
                                  2);
  }

  virtual void TearDown() {
    // Ensure that any unexpected tasks posted by the GPU processor are executed
    // in order to fail the test.
    MessageLoop::current()->RunAllPending();
  }

  base::AtExitManager at_exit_manager;
  MessageLoop message_loop;
  scoped_ptr<MockCommandBuffer> command_buffer_;
  scoped_ptr<::base::SharedMemory> shared_memory_;
  Buffer shared_memory_buffer_;
  int32* buffer_;
  gles2::MockGLES2Decoder* decoder_;
  CommandParser* parser_;
  scoped_ptr<AsyncAPIMock> async_api_;
  scoped_refptr<GPUProcessor> processor_;
};

TEST_F(GPUProcessorTest, ProcessorDoesNothingIfRingBufferIsEmpty) {
  EXPECT_CALL(*command_buffer_, GetPutOffset())
    .WillOnce(Return(0));
  EXPECT_CALL(*command_buffer_, SetGetOffset(0));

  processor_->ProcessCommands();

  EXPECT_EQ(parse_error::kParseNoError,
            command_buffer_->ResetParseError());
  EXPECT_FALSE(command_buffer_->GetErrorStatus());
}

TEST_F(GPUProcessorTest, ProcessesOneCommand) {
  CommandHeader* header = reinterpret_cast<CommandHeader*>(&buffer_[0]);
  header[0].command = 7;
  header[0].size = 2;
  buffer_[1] = 123;

  EXPECT_CALL(*command_buffer_, GetPutOffset())
    .WillOnce(Return(2));
  EXPECT_CALL(*command_buffer_, SetGetOffset(2));

  EXPECT_CALL(*async_api_, DoCommand(7, 1, &buffer_[0]))
    .WillOnce(Return(parse_error::kParseNoError));

  processor_->ProcessCommands();

  EXPECT_EQ(parse_error::kParseNoError,
            command_buffer_->ResetParseError());
  EXPECT_FALSE(command_buffer_->GetErrorStatus());
}

TEST_F(GPUProcessorTest, ProcessesTwoCommands) {
  CommandHeader* header = reinterpret_cast<CommandHeader*>(&buffer_[0]);
  header[0].command = 7;
  header[0].size = 2;
  buffer_[1] = 123;
  header[2].command = 8;
  header[2].size = 1;

  EXPECT_CALL(*command_buffer_, GetPutOffset())
    .WillOnce(Return(3));
  EXPECT_CALL(*command_buffer_, SetGetOffset(3));

  EXPECT_CALL(*async_api_, DoCommand(7, 1, &buffer_[0]))
    .WillOnce(Return(parse_error::kParseNoError));

  EXPECT_CALL(*async_api_, DoCommand(8, 0, &buffer_[2]))
    .WillOnce(Return(parse_error::kParseNoError));

  processor_->ProcessCommands();
}

TEST_F(GPUProcessorTest, ProcessorSetsAndResetsTheGLContext) {
  EXPECT_CALL(*decoder_, MakeCurrent())
    .WillOnce(Return(true));
  EXPECT_CALL(*command_buffer_, GetPutOffset())
    .WillOnce(Return(0));
  EXPECT_CALL(*command_buffer_, SetGetOffset(0));

  processor_->ProcessCommands();
}

TEST_F(GPUProcessorTest, PostsTaskToFinishRemainingCommands) {
  CommandHeader* header = reinterpret_cast<CommandHeader*>(&buffer_[0]);
  header[0].command = 7;
  header[0].size = 2;
  buffer_[1] = 123;
  header[2].command = 8;
  header[2].size = 1;
  header[3].command = 9;
  header[3].size = 1;

  EXPECT_CALL(*command_buffer_, GetPutOffset())
    .WillOnce(Return(4));

  EXPECT_CALL(*async_api_, DoCommand(7, 1, &buffer_[0]))
    .WillOnce(Return(parse_error::kParseNoError));

  EXPECT_CALL(*async_api_, DoCommand(8, 0, &buffer_[2]))
    .WillOnce(Return(parse_error::kParseNoError));

  EXPECT_CALL(*command_buffer_, SetGetOffset(3));

  processor_->ProcessCommands();

  // ProcessCommands is called a second time when the pending task is run.

  EXPECT_CALL(*command_buffer_, GetPutOffset())
    .WillOnce(Return(4));

  EXPECT_CALL(*async_api_, DoCommand(9, 0, &buffer_[3]))
    .WillOnce(Return(parse_error::kParseNoError));

  EXPECT_CALL(*command_buffer_, SetGetOffset(4));

  MessageLoop::current()->RunAllPending();
}

TEST_F(GPUProcessorTest, SetsErrorCodeOnCommandBuffer) {
  CommandHeader* header = reinterpret_cast<CommandHeader*>(&buffer_[0]);
  header[0].command = 7;
  header[0].size = 1;

  EXPECT_CALL(*command_buffer_, GetPutOffset())
    .WillOnce(Return(1));
  EXPECT_CALL(*command_buffer_, SetGetOffset(1));

  EXPECT_CALL(*async_api_, DoCommand(7, 0, &buffer_[0]))
    .WillOnce(Return(
        parse_error::kParseUnknownCommand));

  EXPECT_CALL(*command_buffer_,
      SetParseError(parse_error::kParseUnknownCommand));

  processor_->ProcessCommands();
}

TEST_F(GPUProcessorTest,
       RecoverableParseErrorsAreNotClearedByFollowingSuccessfulCommands) {
  CommandHeader* header = reinterpret_cast<CommandHeader*>(&buffer_[0]);
  header[0].command = 7;
  header[0].size = 1;
  header[1].command = 8;
  header[1].size = 1;

  EXPECT_CALL(*command_buffer_, GetPutOffset())
    .WillOnce(Return(2));
  EXPECT_CALL(*command_buffer_, SetGetOffset(2));

  EXPECT_CALL(*async_api_, DoCommand(7, 0, &buffer_[0]))
    .WillOnce(Return(
        parse_error::kParseUnknownCommand));

  EXPECT_CALL(*async_api_, DoCommand(8, 0, &buffer_[1]))
    .WillOnce(Return(parse_error::kParseNoError));

  EXPECT_CALL(*command_buffer_,
      SetParseError(parse_error::kParseUnknownCommand));

  processor_->ProcessCommands();
}

TEST_F(GPUProcessorTest, UnrecoverableParseErrorsRaiseTheErrorStatus) {
  CommandHeader* header =
      reinterpret_cast<CommandHeader*>(&buffer_[0]);
  header[0].command = 7;
  header[0].size = 1;
  header[1].command = 8;
  header[1].size = 1;

  EXPECT_CALL(*command_buffer_, GetPutOffset())
    .WillOnce(Return(2));

  EXPECT_CALL(*async_api_, DoCommand(7, 0, &buffer_[0]))
    .WillOnce(Return(parse_error::kParseInvalidSize));

  EXPECT_CALL(*command_buffer_,
      SetParseError(parse_error::kParseInvalidSize));

  EXPECT_CALL(*command_buffer_, RaiseErrorStatus());

  processor_->ProcessCommands();
}

TEST_F(GPUProcessorTest, ProcessCommandsDoesNothingAfterUnrecoverableError) {
  EXPECT_CALL(*command_buffer_, GetErrorStatus())
    .WillOnce(Return(true));

  EXPECT_CALL(*command_buffer_, GetPutOffset())
    .Times(0);

  processor_->ProcessCommands();
}

TEST_F(GPUProcessorTest, CanGetAddressOfSharedMemory) {
  EXPECT_CALL(*command_buffer_.get(), GetTransferBuffer(7))
    .WillOnce(Return(shared_memory_buffer_));

  EXPECT_EQ(&buffer_[0], processor_->GetSharedMemoryBuffer(7).ptr);
}

ACTION_P2(SetPointee, address, value) {
  *address = value;
}

TEST_F(GPUProcessorTest, CanGetSizeOfSharedMemory) {
  EXPECT_CALL(*command_buffer_.get(), GetTransferBuffer(7))
    .WillOnce(Return(shared_memory_buffer_));

  EXPECT_EQ(kRingBufferSize, processor_->GetSharedMemoryBuffer(7).size);
}

TEST_F(GPUProcessorTest, SetTokenForwardsToCommandBuffer) {
  EXPECT_CALL(*command_buffer_, SetToken(7));
  processor_->set_token(7);
}

}  // namespace gpu
