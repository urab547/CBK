// Copyright 2026 CBK Project. 注册表与流水线顺序校验的单元测试。
//
// 这些测试不依赖任何真实算法，也不碰文件系统——它们验证的是
// 三个人共用的那层契约。CI 上第一个跑绿的就是它。

#include <algorithm>
#include <memory>
#include <string>
#include <utility>

#include <gtest/gtest.h>

#include "cbk/packer.h"
#include "cbk/stage.h"
#include "cbk/types.h"

namespace {

// ---- 测试替身：一个什么都不做的 Stage，只用来占位 ----
class FakeStage : public cbk::IStage {
public:
    explicit FakeStage(std::string name) : name_(std::move(name)) {}
    std::string Name() const override { return name_; }
    void Process(const uint8_t* data, size_t len, cbk::ISink& out) override {
        out.Write(data, len);
    }
    void Finish(cbk::ISink&) override {}

private:
    std::string name_;
};

class FakeStageFactory : public cbk::IStageFactory {
public:
    FakeStageFactory(std::string name, cbk::StageKind kind)
        : name_(std::move(name)), kind_(kind) {}
    std::string Name() const override { return name_; }
    cbk::StageKind Kind() const override { return kind_; }
    std::unique_ptr<cbk::IStage> CreateForward() override {
        return std::make_unique<FakeStage>(name_);
    }
    std::unique_ptr<cbk::IStage> CreateInverse() override {
        return std::make_unique<FakeStage>(name_);
    }

private:
    std::string name_;
    cbk::StageKind kind_;
};

// 注册一次假算法，供顺序校验的用例使用。
void EnsureFakesRegistered() {
    cbk::StageRegistry& registry = cbk::StageRegistry::Instance();
    if (!registry.Has("fake-compress")) {
        registry.Register(std::make_unique<FakeStageFactory>(
            "fake-compress", cbk::StageKind::kCompress));
    }
    if (!registry.Has("fake-encrypt")) {
        registry.Register(std::make_unique<FakeStageFactory>(
            "fake-encrypt", cbk::StageKind::kEncrypt));
    }
}

TEST(Types, FileTypeToStringCoversEveryValue) {
    EXPECT_STREQ("regular",      cbk::ToString(cbk::FileType::kRegular));
    EXPECT_STREQ("directory",    cbk::ToString(cbk::FileType::kDirectory));
    EXPECT_STREQ("symlink-file", cbk::ToString(cbk::FileType::kSymlinkFile));
    EXPECT_STREQ("symlink-dir",  cbk::ToString(cbk::FileType::kSymlinkDir));
    EXPECT_STREQ("junction",     cbk::ToString(cbk::FileType::kJunction));
    EXPECT_STREQ("hardlink-ref", cbk::ToString(cbk::FileType::kHardlinkRef));
    EXPECT_STREQ("unsupported",  cbk::ToString(cbk::FileType::kUnsupported));
}

TEST(Types, FormatConstantsAreFrozen) {
    // 这些值一旦变了，已经生成的 .cbk 文件就读不了了。
    // 这个测试挂掉不是要你改测试，是要你先问一句"真的要改格式吗"。
    EXPECT_EQ(1, cbk::kFormatVersion);
    EXPECT_EQ(128u, cbk::kFileHeaderSize);
    EXPECT_EQ(64u, cbk::kFooterSize);
    EXPECT_EQ('C', cbk::kMagic[0]);
    EXPECT_EQ('B', cbk::kMagic[1]);
    EXPECT_EQ('K', cbk::kMagic[2]);
    EXPECT_EQ('F', cbk::kMagic[3]);
}

TEST(PackerRegistry, UnknownNameYieldsNullptrInsteadOfCrashing) {
    EXPECT_FALSE(cbk::PackerRegistry::Instance().Has("nope"));
    EXPECT_EQ(nullptr, cbk::PackerRegistry::Instance().Create("nope"));
}

TEST(StageRegistry, RegisteredFactoryIsDiscoverableByKind) {
    EnsureFakesRegistered();
    const auto compressors = cbk::StageRegistry::Instance().Names(cbk::StageKind::kCompress);
    EXPECT_NE(compressors.end(),
              std::find(compressors.begin(), compressors.end(), "fake-compress"));
    const auto ciphers = cbk::StageRegistry::Instance().Names(cbk::StageKind::kEncrypt);
    EXPECT_NE(ciphers.end(), std::find(ciphers.begin(), ciphers.end(), "fake-encrypt"));
}

TEST(PipelineOrder, EmptyPipelineIsValid) {
    EXPECT_TRUE(cbk::ValidatePipelineOrder({}));
}

TEST(PipelineOrder, CompressBeforeEncryptIsValid) {
    EnsureFakesRegistered();
    EXPECT_TRUE(cbk::ValidatePipelineOrder({"fake-compress", "fake-encrypt"}));
}

TEST(PipelineOrder, CompressAfterEncryptIsRejected) {
    // 先加密再压缩：密文是高熵数据，压不动还会变大。
    EnsureFakesRegistered();
    EXPECT_FALSE(cbk::ValidatePipelineOrder({"fake-encrypt", "fake-compress"}));
}

TEST(PipelineOrder, UnknownStageIsRejected) {
    EXPECT_FALSE(cbk::ValidatePipelineOrder({"no-such-algorithm"}));
}

}  // namespace
