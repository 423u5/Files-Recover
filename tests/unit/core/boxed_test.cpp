// Boxed and SharedBox (after P20, L212): optional values held on the heap,
// read like std::optional; a Boxed copy copies the value, SharedBox copies
// share it.

#include "recovery/boxed.hpp"

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace recovery {
namespace {

TEST(BoxedTest, ReadsLikeAnOptional) {
    Boxed<std::string> box;
    EXPECT_FALSE(box.has_value());
    EXPECT_FALSE(static_cast<bool>(box));
    EXPECT_TRUE(box == std::nullopt);
    EXPECT_THROW((void)box.value(), std::bad_optional_access);

    box = std::string("photo.jpg");
    ASSERT_TRUE(box.has_value());
    EXPECT_EQ(*box, "photo.jpg");
    EXPECT_EQ(box->size(), 9U);
    EXPECT_EQ(box.value(), "photo.jpg");

    box.emplace(3, 'x');
    EXPECT_EQ(*box, "xxx");
    box = std::nullopt;
    EXPECT_FALSE(box.has_value());
    box.emplace("again");
    box.reset();
    EXPECT_FALSE(box.has_value());
}

TEST(BoxedTest, TakesAndGivesOptionals) {
    const std::optional<std::string> some = "a";
    const std::optional<std::string> none;
    Boxed<std::string> box = some;
    EXPECT_EQ(*box, "a");
    box = none;
    EXPECT_FALSE(box.has_value());
    box = std::optional<std::string>("b");
    EXPECT_EQ(*box, "b");
    const Boxed<std::string> fromNone = std::nullopt;
    EXPECT_FALSE(fromNone.has_value());
}

TEST(BoxedTest, ACopyCopiesTheValueAndAMoveTakesIt) {
    Boxed<std::vector<int>> box = std::vector<int>{1, 2, 3};
    Boxed<std::vector<int>> copy = box;
    ASSERT_TRUE(copy.has_value());
    EXPECT_NE(&*copy, &*box);
    copy->push_back(4);
    EXPECT_EQ(box->size(), 3U);
    EXPECT_EQ(copy->size(), 4U);
    EXPECT_FALSE(copy == box);
    copy = box;
    EXPECT_TRUE(copy == box);

    const std::vector<int>* held = &*box;
    Boxed<std::vector<int>> taken = std::move(box);
    EXPECT_EQ(&*taken, held);  // moved, not copied
    copy = std::move(taken);
    EXPECT_EQ(&*copy, held);

    Boxed<std::vector<int>> empty;
    copy = empty;
    EXPECT_FALSE(copy.has_value());
    EXPECT_TRUE(copy == empty);
}

TEST(SharedBoxTest, CopiesShareTheValue) {
    SharedBox<std::vector<int>> box;
    EXPECT_FALSE(box.has_value());
    EXPECT_TRUE(box == std::nullopt);
    EXPECT_THROW((void)box.value(), std::bad_optional_access);

    // Its maker fills it in before it is copied.
    std::vector<int>& made = box.emplace();
    made = {1, 2, 3};
    const SharedBox<std::vector<int>> copy = box;
    ASSERT_TRUE(copy.has_value());
    EXPECT_EQ(&*copy, &*box);
    EXPECT_EQ(copy->size(), 3U);
    EXPECT_EQ(copy.value()[2], 3);

    // A new value replaces the box's own; copies keep the one they share.
    box = std::vector<int>{9};
    EXPECT_NE(&*copy, &*box);
    EXPECT_EQ(copy->size(), 3U);
    EXPECT_EQ(box->size(), 1U);
    box = std::nullopt;
    EXPECT_FALSE(box.has_value());
    EXPECT_EQ(copy->size(), 3U);
    const std::vector<int> value{5, 6};
    box = value;
    EXPECT_EQ(*box, value);
    box.reset();
    EXPECT_FALSE(box.has_value());
}

}  // namespace
}  // namespace recovery
