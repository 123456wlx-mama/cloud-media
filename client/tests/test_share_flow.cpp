// ShareFlow 状态机单测：不连服务端，直接喂 ShareXxxRs 的 payload。
#include <gtest/gtest.h>

#include <QByteArray>
#include <QCoreApplication>
#include <QString>

#include <string>

#include "cm/protocol/messages/share.h"
#include "shareapi/ShareFlow.h"

namespace {

QByteArray bytesOf(const std::string& payload) {
    return QByteArray(payload.data(), static_cast<int>(payload.size()));
}

QByteArray createRs(quint32 code, const char* shareCode, const char* accessCode,
                    bool accessRequired, quint64 expireAtMs) {
    cm::protocol::ShareCreateRs rs;
    rs.code = code;
    rs.shareCode = shareCode;
    rs.accessCode = accessCode;
    rs.accessCodeRequired = accessRequired ? 1 : 0;
    rs.expireAtMs = expireAtMs;
    std::string payload;
    rs.encode(&payload);
    return bytesOf(payload);
}

QByteArray openRs(quint32 code, const char* fileName, quint64 size,
                  const char* ticket) {
    cm::protocol::ShareOpenRs rs;
    rs.code = code;
    rs.fileName = fileName;
    rs.size = size;
    rs.shareTicket = ticket;
    rs.fileMd5 = "0123456789abcdef0123456789abcdef";
    std::string payload;
    rs.encode(&payload);
    return bytesOf(payload);
}

}  // namespace

TEST(ShareFlowTest, CreateSuccessFillsCodeAccessAndExpiry) {
    cm::shareapi::ShareFlow flow(nullptr);
    flow.create(30670, 2, 1);
    EXPECT_EQ(cm::shareapi::SharePhase::Creating, flow.info().phase);

    int created = 0;
    QObject::connect(&flow, &cm::shareapi::ShareFlow::created,
                     [&created]() { ++created; });

    flow.applyCreateRs(createRs(0, "ABCDEF", "1234", true, 1761000000000ULL));
    EXPECT_EQ(cm::shareapi::SharePhase::Created, flow.info().phase);
    EXPECT_EQ(QString::fromUtf8("ABCDEF"), flow.info().shareCode);
    EXPECT_EQ(QString::fromUtf8("1234"), flow.info().accessCode);
    EXPECT_TRUE(flow.info().accessCodeRequired);
    EXPECT_EQ(1761000000000ULL, flow.info().expireAtMs);
    EXPECT_EQ(1, created);
}

TEST(ShareFlowTest, CreateRejectedKeepsCodeAndFails) {
    cm::shareapi::ShareFlow flow(nullptr);
    flow.create(1, 2, 1);
    flow.applyCreateRs(createRs(1002, "", "", false, 0));
    EXPECT_EQ(cm::shareapi::SharePhase::Failed, flow.info().phase);
    EXPECT_EQ(1002u, flow.info().code);
    EXPECT_EQ(QString::fromUtf8("分享不存在或已被取消"), flow.info().message);
}

TEST(ShareFlowTest, OpenExpiredShowsExpiredText) {
    cm::shareapi::ShareFlow flow(nullptr);
    flow.openShare(QString::fromUtf8(" abcdef "), QString::fromUtf8("1234"));
    EXPECT_EQ(cm::shareapi::SharePhase::Opening, flow.info().phase);
    // 归一化：去空格 + 大写
    EXPECT_EQ(QString::fromUtf8("ABCDEF"), flow.info().shareCode);

    flow.applyOpenRs(openRs(2003, "", 0, ""));
    EXPECT_EQ(cm::shareapi::SharePhase::Failed, flow.info().phase);
    EXPECT_EQ(QString::fromUtf8("分享已过期"), flow.info().message);
}

TEST(ShareFlowTest, OpenWrongAccessCodeShowsText) {
    cm::shareapi::ShareFlow flow(nullptr);
    flow.openShare(QString::fromUtf8("ABCDEF"), QString::fromUtf8("0000"));
    flow.applyOpenRs(openRs(2004, "", 0, ""));
    EXPECT_EQ(QString::fromUtf8("提取码错误"), flow.info().message);
}

TEST(ShareFlowTest, OpenSuccessExposesTicketForDownload) {
    cm::shareapi::ShareFlow flow(nullptr);
    flow.openShare(QString::fromUtf8("ABCDEF"), QString::fromUtf8("1234"));
    int opened = 0;
    QObject::connect(&flow, &cm::shareapi::ShareFlow::opened,
                     [&opened]() { ++opened; });

    flow.applyOpenRs(openRs(0, "p82_big.bin", 20971520, "9f8e7d6c5b4a39281706f5e4d3c2b1a0"));
    EXPECT_EQ(cm::shareapi::SharePhase::Opened, flow.info().phase);
    EXPECT_EQ(QString::fromUtf8("p82_big.bin"), flow.info().fileName);
    EXPECT_EQ(20971520ULL, flow.info().size);
    EXPECT_EQ(QString::fromUtf8("9f8e7d6c5b4a39281706f5e4d3c2b1a0"), flow.info().shareTicket);
    EXPECT_EQ(1, opened);
}

TEST(ShareFlowTest, EmptyCodeRejectedBeforeSend) {
    cm::shareapi::ShareFlow flow(nullptr);
    flow.openShare(QString::fromUtf8("   "), QString());
    EXPECT_EQ(cm::shareapi::SharePhase::Failed, flow.info().phase);
    EXPECT_EQ(QString::fromUtf8("请输入分享码"), flow.info().message);
}

TEST(ShareFlowTest, CancelSuccessMovesToCancelled) {
    cm::shareapi::ShareFlow flow(nullptr);
    flow.applyCancelRs(QByteArray());
    EXPECT_EQ(cm::shareapi::SharePhase::Failed, flow.info().phase);  // 空 payload 解析失败

    cm::protocol::ShareCancelRs rs;
    rs.code = 0;
    rs.cancelled = 1;
    std::string payload;
    rs.encode(&payload);
    flow.applyCancelRs(bytesOf(payload));
    EXPECT_EQ(cm::shareapi::SharePhase::Cancelled, flow.info().phase);
}

TEST(ShareFlowTest, ErrorCodeTextMapping) {
    EXPECT_EQ(QString::fromUtf8("分享已过期"),
              cm::shareapi::ShareFlow::errorText(2003));
    EXPECT_EQ(QString::fromUtf8("提取码错误"),
              cm::shareapi::ShareFlow::errorText(2004));
    EXPECT_EQ(QString::fromUtf8("永久有效"),
              cm::shareapi::ShareFlow::expiryText(0));
}
