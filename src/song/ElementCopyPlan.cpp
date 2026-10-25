// SPDX-License-Identifier: GPL-3.0-or-later
#include "crosspad/song/ElementCopyPlan.hpp"

#include <algorithm>
#include <cstdlib>
#include <set>

namespace crosspad {

namespace {

/// What an untitled element's copies are counted from.
constexpr char kUntitled[] = "ELEMENT";
/// The most digits a title's number is read from: a longer run is part of the name.
constexpr size_t kMaxTitleDigits = 6;

bool contains(const std::vector<std::string>& v, const std::string& s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}

/// Every element @p song plays, through any number of others.
std::set<std::string> songTree(const std::string& song, const ElementUses& uses) {
    std::set<std::string> tree;
    for (bool grew = true; grew;) {
        grew = false;
        for (const auto& kv : uses) {
            if (tree.count(kv.first)) continue;
            for (const ElementUse& u : kv.second) {
                if (u.song ? u.user == song : tree.count(u.user) != 0) {
                    tree.insert(kv.first);
                    grew = true;
                    break;
                }
            }
        }
    }
    return tree;
}

/// The elements of @p tree heard only through @p song: every user is the song
/// or one of them. An element anything else plays -- another song, an element
/// outside the tree, one heard elsewhere -- and everything under it is not.
std::set<std::string> heardOnlyIn(const std::string& song, const ElementUses& uses, std::set<std::string> tree) {
    for (bool shrank = true; shrank;) {
        shrank = false;
        for (auto it = tree.begin(); it != tree.end();) {
            const auto found = uses.find(*it);
            bool outside = false;
            if (found != uses.end()) {
                for (const ElementUse& u : found->second)
                    outside = outside || (u.song ? u.user != song : tree.count(u.user) == 0);
            }
            if (outside) {
                it = tree.erase(it);
                shrank = true;
            } else {
                ++it;
            }
        }
    }
    return tree;
}

} // namespace

ElementCopyPlan plan_element_copies(const std::string& song, const ElementUses& uses,
                                    const std::vector<std::string>& seeds,
                                    const std::vector<std::string>& copied) {
    ElementCopyPlan plan;
    const std::set<std::string> tree = songTree(song, uses);
    const std::set<std::string> own = heardOnlyIn(song, uses, tree);
    /* An original being copied keeps what it plays: its copy takes the new refs. */
    std::set<std::string> leaving(copied.begin(), copied.end());
    std::vector<std::string> queue = copied;
    /* A seed is copied as asked, even one copied for the song before: its
     * holders are found once either way. */
    for (const std::string& s : seeds) {
        if (contains(plan.forks, s)) continue;
        plan.forks.push_back(s);
        if (leaving.insert(s).second) queue.push_back(s);
    }
    for (size_t i = 0; i < queue.size(); ++i) {
        const auto found = uses.find(queue[i]);
        if (found == uses.end()) continue;
        for (const ElementUse& u : found->second) {
            if (u.song) {
                plan.song = plan.song || u.user == song;
                continue;
            }
            if (!tree.count(u.user) || leaving.count(u.user)) continue;
            if (own.count(u.user)) {
                if (!contains(plan.rewrites, u.user)) plan.rewrites.push_back(u.user);
                continue;
            }
            leaving.insert(u.user);
            plan.forks.push_back(u.user);
            queue.push_back(u.user);
        }
    }
    return plan;
}

std::string element_copy_title(const std::string& title, const std::vector<std::string>& taken) {
    std::string stem = title.empty() ? kUntitled : title;
    unsigned long n = 1;
    const size_t space = stem.find_last_of(' ');
    const size_t digits = space == std::string::npos ? 0 : stem.size() - space - 1;
    if (digits && digits <= kMaxTitleDigits &&
        std::all_of(stem.begin() + (long)space + 1, stem.end(), [](char c) { return c >= '0' && c <= '9'; })) {
        n = std::strtoul(stem.c_str() + space + 1, nullptr, 10);
        stem.resize(space);
    }
    for (unsigned long k = n + 1;; ++k) {
        std::string candidate = stem + " " + std::to_string(k);
        if (!contains(taken, candidate)) return candidate;
    }
}

} // namespace crosspad
