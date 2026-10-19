// Tick Input Buffer
//
// 1) Remembers a jump click for N physics ticks (N = 1, 2, 4, 8, 16) and fires it
//    on the first tick the player is actually able to act on it.
// 2) Stamps the window used on the level complete screen.
//
// Not compiled or tested against 2.2081 by the author. If the build fails, the
// lines most likely to need renaming are marked  // VERIFY

#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/EndLevelLayer.hpp>

#include <array>
#include <string>
#include <type_traits>

using namespace geode::prelude;

namespace {
	constexpr int kJump = 1;

	// Window snapshotted when the attempt starts, so changing the setting
	// mid-attempt can't make the watermark lie about the run.
	int s_runWindow = 4;

	struct Channel {
		bool held = false;       // physical state of the jump button
		bool pending = false;    // a click is waiting for the player to be able to use it
		int expireTick = 0;      // last tick the pending click may still fire
		int releaseTick = -1;    // when to release a click we injected after the user let go
	};

	std::array<Channel, 2> s_ch{};  // [0] = player 1 input, [1] = player 2 input
	int s_lastTick = -1;
	bool s_internal = false;        // true while we inject our own input

	int readWindow() {
		auto raw = Mod::get()->getSettingValue<std::string>("buffer-ticks");
		int v = 4;
		try { v = std::stoi(raw); } catch (...) {}
		switch (v) {
			case 1: case 2: case 4: case 8: case 16: return v;
			default: return 4;
		}
	}

	void resetState() {
		s_ch = {};
		s_lastTick = -1;
	}

	// Works whether the field is a CCArray* or a std/gd vector.
	template <class T>
	bool nonEmpty(T const& c) {
		if constexpr (std::is_pointer_v<T>) return c != nullptr && c->count() > 0;
		else return c.size() > 0;
	}

	// Ship, UFO, wave and swing react to every click immediately, so there is
	// nothing to buffer. Cube, ball, robot and spider only act when grounded
	// or on an orb.
	bool isConditionalMode(PlayerObject* p) {
		return !(p->m_isShip || p->m_isBird || p->m_isDart || p->m_isSwing);
	}

	bool canAct(PlayerObject* p) {
		if (p->m_isDead) return false;
		return p->m_isOnGround || nonEmpty(p->m_touchingRings); // VERIFY m_touchingRings
	}

	struct Probe {
		bool conditional = false;  // at least one controlled player needs buffering
		bool canAct = false;       // at least one of them can act right now
	};

	Probe probe(GJBaseGameLayer* l, int ch) {
		Probe r;
		auto add = [&](PlayerObject* p) {
			if (!p || !isConditionalMode(p)) return;
			r.conditional = true;
			if (canAct(p)) r.canAct = true;
		};
		bool dual = l->m_gameState.m_isDualMode;                                   // VERIFY
		bool twoP = l->m_levelSettings && l->m_levelSettings->m_twoPlayerMode;    // VERIFY
		if (ch == 0) {
			add(l->m_player1);
			if (dual && !twoP) add(l->m_player2);
		} else if (dual && twoP) {
			add(l->m_player2);
		}
		return r;
	}

	int currentTick(GJBaseGameLayer* l) {
		return l->m_gameState.m_currentProgress;  // VERIFY
	}

	bool isPlaying(GJBaseGameLayer* l) {
		return static_cast<GJBaseGameLayer*>(PlayLayer::get()) == l;
	}

	void inject(GJBaseGameLayer* l, bool down, bool isPlayer1) {
		s_internal = true;
		l->handleButton(down, kJump, isPlayer1);
		s_internal = false;
	}

	void tickBuffers(GJBaseGameLayer* l) {
		int tick = currentTick(l);
		if (tick == s_lastTick) return;  // half-tick sub-calls, don't double count
		s_lastTick = tick;

		for (int ch = 0; ch < 2; ++ch) {
			auto& c = s_ch[ch];
			bool isP1 = ch == 0;

			if (c.releaseTick >= 0 && tick >= c.releaseTick) {
				c.releaseTick = -1;
				if (!c.held) inject(l, false, isP1);
			}

			if (!c.pending) continue;
			if (tick > c.expireTick) {
				c.pending = false;
				continue;
			}

			if (probe(l, ch).canAct) {
				c.pending = false;
				// Release first so the game always sees a fresh press edge,
				// even if the user is still physically holding the button.
				inject(l, false, isP1);
				inject(l, true, isP1);
				// User already let go: turn the buffered click into a 1-tick tap
				// so the game doesn't think the button is stuck down.
				if (!c.held) c.releaseTick = tick + 1;
			}
		}
	}
}

class $modify(TIBGameLayer, GJBaseGameLayer) {
	void handleButton(bool down, int button, bool isPlayer1) {
		if (s_internal || button != kJump || !isPlaying(this)) {
			return GJBaseGameLayer::handleButton(down, button, isPlayer1);
		}

		auto& c = s_ch[isPlayer1 ? 0 : 1];
		c.held = down;

		// Judge readiness BEFORE the real click changes player state.
		bool needBuffer = false;
		if (down) {
			auto p = probe(this, isPlayer1 ? 0 : 1);
			needBuffer = p.conditional && !p.canAct;
		}

		GJBaseGameLayer::handleButton(down, button, isPlayer1);

		if (down) {
			c.pending = needBuffer;
			c.expireTick = currentTick(this) + s_runWindow;
		}
		// A release does NOT cancel a pending click: tapping early and letting go
		// is the whole point of a buffer.
	}

	void processCommands(float dt, bool isHalfTick, bool isLastTick) {
		if (isPlaying(this)) tickBuffers(this);
		GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
	}
};

class $modify(TIBPlayLayer, PlayLayer) {
	void resetLevel() {
		PlayLayer::resetLevel();
		s_runWindow = readWindow();
		resetState();
	}

	void onQuit() {
		resetState();
		PlayLayer::onQuit();
	}
};

class $modify(TIBEndLevelLayer, EndLevelLayer) {
	void customSetup() {
		EndLevelLayer::customSetup();

		auto win = CCDirector::get()->getWinSize();
		auto text = fmt::format("Input Buffer: {} tick{}", s_runWindow, s_runWindow == 1 ? "" : "s");

		auto label = CCLabelBMFont::create(text.c_str(), "bigFont.fnt");
		label->setScale(0.3f);
		label->setOpacity(160);
		label->setPosition({win.width / 2.f, 9.f});
		label->setID("buffer-watermark"_spr);
		m_mainLayer->addChild(label, 100);
	}
};
