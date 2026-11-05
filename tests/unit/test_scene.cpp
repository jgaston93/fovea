#include <memory>
#include <thread>

#include <gtest/gtest.h>

#include "fovea/core/scene.hpp"
#include "support/alloc_counter.hpp"

namespace fovea {
namespace {

TEST(SceneState, NothingPublishedReadsAsSequenceZero) {
    auto scene = std::make_unique<SceneState>();
    EXPECT_EQ(scene->latest().sequence, 0U);
}

TEST(SceneState, ReaderSeesTheLatestPublishAndEditsKeepTheirState) {
    auto scene = std::make_unique<SceneState>();
    scene->edit().environment.time_of_day_h = 6.5;
    scene->publish();
    EntityState e;
    e.id = 7;
    ASSERT_TRUE(scene->edit().entities.push_back(e));
    scene->publish();

    const SceneSnapshot& s = scene->latest();
    EXPECT_EQ(s.sequence, 2U);
    EXPECT_EQ(s.environment.time_of_day_h, 6.5);  // kept from the first edit
    ASSERT_EQ(s.entities.size(), 1U);
    EXPECT_EQ(s.entities[0].id, 7U);
}

TEST(SceneState, FindEntityById) {
    SceneSnapshot s;
    EntityState a;
    a.id = 3;
    EntityState b;
    b.id = 9;
    b.model = 5;
    ASSERT_TRUE(s.entities.push_back(a));
    ASSERT_TRUE(s.entities.push_back(b));
    ASSERT_NE(find_entity(s, 9), nullptr);
    EXPECT_EQ(find_entity(s, 9)->model, 5U);
    EXPECT_EQ(find_entity(s, 4), nullptr);
}

TEST(SceneState, UnpublishedEditsAreNotVisible) {
    auto scene = std::make_unique<SceneState>();
    scene->publish();
    scene->edit().environment.time_of_day_h = 20.0;
    EXPECT_EQ(scene->latest().environment.time_of_day_h, 12.0);
}

TEST(SceneState, PublishAndReadAllocateNothing) {
    auto scene = std::make_unique<SceneState>();
    const test::AllocationCounter counter;
    for (int i = 0; i < 10; ++i) {
        scene->edit().environment.time_of_day_h = i;
        scene->publish();
        EXPECT_EQ(scene->latest().environment.time_of_day_h, i);
    }
    EXPECT_EQ(counter.count(), 0U);
}

TEST(SceneState, ConcurrentReaderSeesWholeSnapshotsInOrder) {
    // The writer keeps time_of_day_h equal to the entity count; a torn
    // snapshot would break that.
    auto scene = std::make_unique<SceneState>();
    constexpr int kPublishes = 3000;
    std::thread writer([&] {
        for (int i = 1; i <= kPublishes; ++i) {
            SceneSnapshot& s = scene->edit();
            s.entities.clear();
            for (int k = 0; k < i % 50; ++k) {
                (void)s.entities.push_back({});
            }
            s.environment.time_of_day_h = static_cast<double>(i % 50);
            scene->publish();
        }
    });
    std::uint64_t last = 0;
    int torn = 0;
    int older = 0;
    while (last < kPublishes) {
        const SceneSnapshot& s = scene->latest();
        if (s.sequence == 0) {
            continue;  // the default snapshot, before the first publish
        }
        torn += (static_cast<double>(s.entities.size()) != s.environment.time_of_day_h) ? 1 : 0;
        older += (s.sequence < last) ? 1 : 0;
        last = s.sequence;
    }
    writer.join();
    EXPECT_EQ(torn, 0);
    EXPECT_EQ(older, 0);
}

}  // namespace
}  // namespace fovea
