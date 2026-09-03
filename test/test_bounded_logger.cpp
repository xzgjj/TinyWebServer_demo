//有界异步日志队列单元测试：验证 DISCARD_OLDEST / DISCARD_NEWEST / kBlock 三种满队列策略
//与丢弃计数语义（丢弃计数单位为"缓冲"，每个缓冲约一条 2.2MB 大日志，便于精确断言）。

#include "async_logger.h"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <string>
#include <thread>

namespace {

constexpr const char* kLogPath = "./test_bounded_logger.log";
constexpr size_t kLineSize = 2'200'000;  // ~2.2MB/行：单缓冲（4MB）恰好一条，换出时 1 行 = 1 缓冲

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                         \
    do {                                                                    \
        ++g_checks;                                                         \
        if (!(cond)) {                                                      \
            ++g_failures;                                                   \
            std::printf("FAIL: %s (line %d)\n", #cond, __LINE__);           \
        }                                                                   \
    } while (0)

/** 构造长度为 size 的日志行：头部可读 + 'x' 填充 + 换行 */
std::string MakeLine(int index, size_t size) {
    std::string s = "BOUNDED LOG LINE " + std::to_string(index) + " ";
    if (size <= s.size()) return s + "\n";
    s.append(size - s.size() - 1, 'x');
    s.push_back('\n');
    return s;
}

/** 统计日志文件中的行数（按 '\n' 计） */
size_t CountLines(const char* path) {
    std::ifstream in(path);
    if (!in) return 0;
    std::string line;
    size_t n = 0;
    while (std::getline(in, line)) ++n;
    return n;
}

// ============================================================================
// 场景 1：DiscardOldest —— 队列满时丢弃最旧缓冲，保留最新
// ============================================================================
void TestDiscardOldest() {
    std::remove(kLogPath);
    {
        AsyncLogger logger(kLogPath, /*flush_interval=*/1,
                           /*max_pending=*/3,
                           AsyncLogger::FullPolicy::kDiscardOldest,
                           /*max_file_size=*/0);
        logger.Start();

        const int kLines = 30;
        for (int i = 0; i < kLines; ++i) {
            std::string line = MakeLine(i, kLineSize);
            // 大行强制换缓冲（1 行 = 1 个队列缓冲），快速灌入制造队列堆积
            logger.Append(line.data(), line.size());
        }

        logger.Flush();
        logger.Stop();
        const int64_t dropped = logger.GetDroppedCount();

        // 队列上限 3：30 条中后端写盘期间必有丢弃
        CHECK(dropped > 0);
        // 每条丢弃 = 一个缓冲 = 一行日志（1 行/缓冲语义下精确成立）
        CHECK(CountLines(kLogPath) == static_cast<size_t>(kLines - dropped));
        std::printf("  TestDiscardOldest: lines=%zu dropped=%lld\n",
                    CountLines(kLogPath), static_cast<long long>(dropped));
    }
    std::remove(kLogPath);
}

// ============================================================================
// 场景 2：DiscardNewest —— 队列满时丢弃最新日志行
// ============================================================================
void TestDiscardNewest() {
    std::remove(kLogPath);
    {
        AsyncLogger logger(kLogPath, /*flush_interval=*/1,
                           /*max_pending=*/3,
                           AsyncLogger::FullPolicy::kDiscardNewest,
                           /*max_file_size=*/0);
        logger.Start();

        const int kLines = 30;
        for (int i = 0; i < kLines; ++i) {
            std::string line = MakeLine(i, kLineSize);
            logger.Append(line.data(), line.size());
        }

        logger.Flush();
        logger.Stop();
        const int64_t dropped = logger.GetDroppedCount();

        CHECK(dropped > 0);
        CHECK(CountLines(kLogPath) == static_cast<size_t>(kLines - dropped));
        std::printf("  TestDiscardNewest: lines=%zu dropped=%lld\n",
                    CountLines(kLogPath), static_cast<long long>(dropped));
    }
    std::remove(kLogPath);
}

// ============================================================================
// 场景 3：kBlock —— 队列满时等待后端腾出空间，不丢日志（有界等待 1s 兜底）
// ============================================================================
void TestBlock() {
    std::remove(kLogPath);
    {
        AsyncLogger logger(kLogPath, /*flush_interval=*/1,
                           /*max_pending=*/1,
                           AsyncLogger::FullPolicy::kBlock,
                           /*max_file_size=*/0);
        logger.Start();

        // 逐条写入并让后端有时间消化，验证"等待但不丢行"路径
        const int kLines = 4;
        for (int i = 0; i < kLines; ++i) {
            std::string line = MakeLine(i, kLineSize);
            logger.Append(line.data(), line.size());
            std::this_thread::sleep_for(std::chrono::milliseconds(120));
        }

        logger.Flush();
        logger.Stop();

        CHECK(logger.GetDroppedCount() == 0);
        CHECK(CountLines(kLogPath) == kLines);
        std::printf("  TestBlock: lines=%zu dropped=%lld\n",
                    CountLines(kLogPath),
                    static_cast<long long>(logger.GetDroppedCount()));
    }
    std::remove(kLogPath);
}

// ============================================================================
// 场景 4：小日志回归 —— 正常路径不得误丢弃
// ============================================================================
void TestSmallLogsNoDrop() {
    std::remove(kLogPath);
    {
        AsyncLogger logger(kLogPath, /*flush_interval=*/1,
                           /*max_pending=*/2,
                           AsyncLogger::FullPolicy::kDiscardOldest,
                           /*max_file_size=*/0);
        logger.Start();

        const int kLines = 5000;
        std::string line = "SMALL LOG LINE 0000\n";  // 复用同一行内容
        for (int i = 0; i < kLines; ++i) {
            // 模拟不同行号（内容变化）
            if (line[15] == '9') line[15] = '0';
            else ++line[15];
            logger.Append(line.data(), line.size());
        }

        logger.Flush();
        logger.Stop();

        // 小日志全部落在 4MB 当前缓冲内，不会触发换出/丢弃
        CHECK(logger.GetDroppedCount() == 0);
        CHECK(logger.GetPendingCount() == 0);
        std::printf("  TestSmallLogsNoDrop: lines=%zu dropped=%lld pending=%zu\n",
                    CountLines(kLogPath),
                    static_cast<long long>(logger.GetDroppedCount()),
                    logger.GetPendingCount());
    }
    std::remove(kLogPath);
}

// ============================================================================
// 场景 5：日志文件轮转 —— 超过阈值后保存为 .1 并重开新文件（保留 1 份旧文件）
// ============================================================================
void TestRotation() {
    std::string rotated = std::string(kLogPath) + ".1";
    std::remove(kLogPath);
    std::remove(rotated.c_str());
    {
        AsyncLogger logger(kLogPath, /*flush_interval=*/1,
                           /*max_pending=*/8,
                           AsyncLogger::FullPolicy::kDiscardOldest,
                           /*max_file_size=*/5 * 1024 * 1024);  // 5MB 阈值
        logger.Start();

        // 3 条 2.2MB ≈ 6.6MB > 5MB，写盘累计必然触发一次轮转
        for (int i = 0; i < 3; ++i) {
            std::string line = MakeLine(i, kLineSize);
            logger.Append(line.data(), line.size());
        }

        logger.Flush();
        logger.Stop();

        size_t cur = CountLines(kLogPath);        // 轮转后新文件
        size_t old = CountLines(rotated.c_str()); // 轮转保存的 .1 文件
        CHECK(logger.GetDroppedCount() == 0);
        // 总行数守恒（3 行，轮转不丢数据）
        CHECK(cur + old == 3);
        // 阈值 5MB：前 2 条（4.4MB）在旧文件，第 3 条触发轮转 → 旧文件至少 2 行
        CHECK(old >= 2);
        std::printf("  TestRotation: cur=%zu rotated=%zu dropped=%lld\n",
                    cur, old, static_cast<long long>(logger.GetDroppedCount()));
    }
    std::remove(kLogPath);
    std::remove(rotated.c_str());
}

// ============================================================================
// 场景 6：并发压测 —— 8 线程 × 5 条大行，丢弃数守恒（每丢 1 缓冲 = 1 行）
// ============================================================================
void TestConcurrentDiscardOldest() {
    std::remove(kLogPath);
    {
        AsyncLogger logger(kLogPath, /*flush_interval=*/1,
                           /*max_pending=*/4,
                           AsyncLogger::FullPolicy::kDiscardOldest,
                           /*max_file_size=*/0);
        logger.Start();

        const int kThreads = 8;
        const int kLinesPerThread = 5;
        std::vector<std::thread> threads;
        for (int t = 0; t < kThreads; ++t) {
            threads.emplace_back([&logger, t]() {
                for (int j = 0; j < kLinesPerThread; ++j) {
                    std::string line = MakeLine(t * 1000 + j, kLineSize);
                    logger.Append(line.data(), line.size());
                }
            });
        }
        for (auto& th : threads) th.join();

        logger.Flush();
        logger.Stop();
        const int64_t dropped = logger.GetDroppedCount();

        CHECK(dropped > 0);
        CHECK(CountLines(kLogPath) ==
              static_cast<size_t>(kThreads * kLinesPerThread - dropped));
        std::printf("  TestConcurrentDiscardOldest: lines=%zu dropped=%lld\n",
                    CountLines(kLogPath), static_cast<long long>(dropped));
    }
    std::remove(kLogPath);
}

}  // namespace

int main() {
    std::printf("=== bounded async logger tests ===\n");

    TestDiscardOldest();
    TestDiscardNewest();
    TestBlock();
    TestSmallLogsNoDrop();
    TestRotation();
    TestConcurrentDiscardOldest();

    std::printf("=== %d checks, %d failures ===\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
