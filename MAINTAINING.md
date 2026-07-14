# Maintaining morrownr/mt76

How patches get into this tree, written plain so anyone lending a hand can do it without breaking things. If something here doesn't match what you're actually seeing in the repo, trust the repo, not the doc.

## What lands here, and where it comes from

Three kinds of change end up in the tree:

1. Our own work. The install and uninstall scripts, the dkms.conf, the README, firmware updates, and the occasional driver fix we write ourselves.
2. Commits pulled from openwrt/mt76. That's OpenWrt's mt76 tree, the one Felix Fietkau maintains, and it's where most mt76 bug fixes and new-chip support turn up first. We take the fixes we want one at a time instead of merging the whole thing, because our tree builds standalone and carries its own compat guards, and a wholesale merge would fight both.
3. Kernel compat. Small guards in the driver code that keep the tree building on older kernels. These live in the source, not as separate patch files. More on that below.

## Getting a change in

Two ways, depending on who's doing it and whether it's ready.

Straight to main is how @morrownr works day to day. Edit, commit, push. If it turns out wrong, revert it and try again. No branches. It's the right call for small, obvious fixes where a review would just slow things down.

    git add <files>
    git commit -m "short description of the change"
    git push

For anything bigger, or anything you'd want a second set of eyes on before it's live, don't push it to main yourself. Either send the diff to @morrownr and let him put it on main, or open a pull request so it can be looked at in place. Sending the diff is the better move when the work isn't ready for the public yet.

That last point matters because the repo is public. The second you push a branch or open a PR, anyone can see it. So if a change should stay private until it's done, keep it off the repo and send the diff instead.

If you do want a PR, it's the branch you push to that keeps it from merging:

    git checkout -b some-fix
    git add <files>
    git commit -m "short description of the change"
    git push -u origin some-fix

The branch name is just a label. Nothing merges on its own. GitHub shows a "Compare & pull request" button, and the PR stays open until someone merges it.

## Getting an unfinished fix to a tester

Now and then you've got a fix that isn't ready to merge but you want one person to try it, a reporter on the exact adapter that's failing, say. Push it as a branch and point them at it so they can clone that branch and install, or build it and send them the module directly. It stays off main until their result says it's good.

## Pulling a commit from openwrt/mt76

Most of the real driver work comes from here. Point git at openwrt once, find the commit you want, copy it onto our tree, build-test it, push.

One-time setup:

    git remote add openwrt https://github.com/openwrt/mt76.git
    git fetch openwrt

fetch just pulls their commits into your copy. It doesn't change any of your files and it doesn't touch main.

Find what's worth taking:

    # what openwrt has that we don't
    git log --oneline main..openwrt/master

    # only the commits touching a driver you care about
    git log --oneline openwrt/master -- mt7921/

Their branch is master, ours is main. We never check out their branch, we just borrow one commit off it.

Copy the commit over:

    git cherry-pick -x -s <commit-id>

cherry-pick makes a fresh copy of that one commit on top of where you are. It gets a new ID because it's sitting on a different parent. The two flags earn their keep:

- -x records a "cherry picked from commit ..." line, so anyone can trace it back to openwrt.
- -s adds your Signed-off-by line. Set your name and email once with git config --global user.name and git config --global user.email, and the sign-off uses those.

Short commit id or long, git takes any unique piece of it. Use the 12-character version when you write it down somewhere, it's long enough not to get mistaken for anything else.

When a cherry-pick doesn't apply cleanly, it's almost always one of two shapes. Either openwrt changed a function we've wrapped in a compat guard, in which case keep their change and put the guard back around the new version. Or openwrt touched code we'd already patched a different way, which is rarer, and you look at both and decide what to keep.

## Keeping it building on old kernels

The tree has to build all the way down to kernel 6.12, because that's what current Debian ships and plenty of people are on it. When the kernel changes the shape of something between 6.12 and now, we guard it right in the code:

    /* compat: radio_idx added to ieee80211_ops in kernel 6.17 */
    #if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 17, 0)
    static int mt7921_config(struct ieee80211_hw *hw, int radio_idx, u32 changed)
    #else
    static int mt7921_config(struct ieee80211_hw *hw, u32 changed)
    #endif

Three habits keep these from getting away from us:

- Put a /* compat: ... */ comment one line above every guard. Then you can find all of them at once with grep -rn '/\* compat:' .
- The newer signature goes on top, the older one underneath.
- When we drop support for a kernel old enough that a guard isn't needed anymore, grep the comment and delete them in one go.

No patch files, no configure step. The compiler picks the right side of the guard from the kernel headers it's building against.

## Test before it lands

The least you should do before pushing anything:

- Build on the floor, kernel 6.12. A Pi on current Debian is an easy one to keep around for this.
- Build on something recent, 6.17 or newer.

    make clean && make -j$(nproc)

Zero errors, zero warnings. If the compiler starts warning on code you just touched, that's usually the sign a cherry-pick conflict went sideways. Go back and look before you push.

## About the compat-patches/ folder

Don't let the name fool you. Nothing in the build applies those. There's no step in the Makefile or the install script that runs patch against them. The one file in there is a record, it writes down the inline compat edits that went into the eeprom files for the 6.6 header rename so there's a trail. The real compat work lives in the code as guards, not as patch files in that folder.
