#include "PresentationState.hpp"

#include <cassert>
#include <cstdio>

static PresentationState::Config config(std::uint64_t bufferGeneration,
                                        std::uint64_t configGeneration, int size) {
    PresentationState::Config value;
    value.valid            = true;
    value.bufferGeneration = bufferGeneration;
    value.configGeneration = configGeneration;
    value.sourceRect       = QRectF(0, 0, size, size);
    value.destRect         = QRectF(0, 0, size, size);
    value.clearColor       = QColor::fromRgbF(0.1, 0.2, 0.3, 1.0);
    return value;
}

static PresentationState::Content incoming(PresentationState& state, std::uint64_t generation) {
    PresentationState::Content value;
    assert(state.incomingFor(generation, value));
    return value;
}

static void test_keeps_presented_content_until_commit() {
    PresentationState state;
    state.beginIncoming(1, 1920, 1080, 10, true);
    assert(state.applyConfig(config(1, 1, 1920)) == PresentationState::ConfigResult::Staged);
    assert(state.commit(incoming(state, 1)) == PresentationState::CommitResult::SourceChanged);

    state.beginIncoming(2, 3840, 2160, 10, true);
    assert(state.applyConfig(config(2, 2, 3840)) == PresentationState::ConfigResult::Staged);
    const auto old = state.presented();
    assert(old.bufferGeneration == 1);
    assert(old.width == 1920);
    assert(old.config.sourceRect.width() == 1920);

    assert(state.commit(incoming(state, 2)) == PresentationState::CommitResult::SourceChanged);
    const auto next = state.presented();
    assert(next.bufferGeneration == 2);
    assert(next.width == 3840);
    assert(next.config.sourceRect.width() == 3840);
}

static void test_skipped_generation_never_becomes_presented() {
    PresentationState state;
    state.beginIncoming(1, 100, 100, 10, true);
    (void)state.applyConfig(config(1, 1, 100));
    (void)state.commit(incoming(state, 1));

    state.beginIncoming(2, 200, 200, 10, true);
    (void)state.applyConfig(config(2, 2, 200));
    const auto skipped = incoming(state, 2);
    state.retireIncoming(2);
    state.beginIncoming(3, 300, 300, 10, true);
    (void)state.applyConfig(config(3, 3, 300));
    assert(state.commit(skipped) == PresentationState::CommitResult::Rejected);
    assert(state.presented().bufferGeneration == 1);
    assert(state.commit(incoming(state, 3)) == PresentationState::CommitResult::SourceChanged);
    assert(state.presented().bufferGeneration == 3);
}

static void test_config_only_update_does_not_change_source() {
    PresentationState state;
    state.beginIncoming(4, 400, 400, 10, true);
    (void)state.applyConfig(config(4, 4, 400));
    (void)state.commit(incoming(state, 4));

    auto updated = config(4, 5, 200);
    assert(state.applyConfig(updated) == PresentationState::ConfigResult::PresentedUpdated);
    assert(! state.sourceChangesWith(4));
    assert(state.presented().config.configGeneration == 5);
}

static void test_same_source_commit_is_distinct_from_rejection() {
    PresentationState state;
    state.beginIncoming(6, 600, 600, 10, true);
    (void)state.applyConfig(config(6, 6, 600));
    assert(state.commit(incoming(state, 6)) == PresentationState::CommitResult::SourceChanged);
    assert(state.commit(incoming(state, 6)) == PresentationState::CommitResult::SameSourceUpdated);
}

static void test_invalid_commit_preserves_presented_content() {
    PresentationState state;
    state.beginIncoming(5, 500, 500, 10, true);
    (void)state.applyConfig(config(5, 5, 500));
    (void)state.commit(incoming(state, 5));

    PresentationState::Content invalid;
    assert(state.commit(invalid) == PresentationState::CommitResult::Rejected);
    assert(state.presented().bufferGeneration == 5);
}

int main() {
    test_keeps_presented_content_until_commit();
    test_skipped_generation_never_becomes_presented();
    test_config_only_update_does_not_change_source();
    test_same_source_commit_is_distinct_from_rejection();
    test_invalid_commit_preserves_presented_content();
    std::puts("test_presentation_state: OK");
    return 0;
}
