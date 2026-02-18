/**
 * @file core.h
 * @brief CrossPad Core — master include
 *
 * Include this single header to get all portable CrossPad interfaces.
 * Platform implementations are NOT included here — they live in
 * platform/<target>/ and must be included separately.
 */
#pragma once

// Event system
#include "crosspad/event/EventTypes.hpp"
#include "crosspad/event/EventData.hpp"
#include "crosspad/event/IEventBus.hpp"

// App system
#include "crosspad/app/AppEntry.hpp"
#include "crosspad/app/AppRegistry.hpp"
#include "crosspad/app/AppRegistrar.hpp"

// Settings system
#include "crosspad/settings/CrosspadSettings.hpp"
#include "crosspad/settings/IKeyValueStore.hpp"

// Protocol
#include "crosspad/protocol/SysExProtocol.h"
#include "crosspad/protocol/IStm32Transport.hpp"
#include "crosspad/protocol/SysExFramer.hpp"
#include "crosspad/protocol/Stm32CommandSender.hpp"
#include "crosspad/protocol/Stm32MessageHandler.hpp"

// Status
#include "crosspad/status/CrosspadStatus.hpp"

// Color types
#include "crosspad/led/HslColor.hpp"
#include "crosspad/led/RgbColor.hpp"

// Pad system
#include "crosspad/pad/Pad.hpp"
#include "crosspad/pad/IPadLogicHandler.hpp"
#include "crosspad/pad/RGBCallbackManager.hpp"

// LED types and interfaces
#include "crosspad/led/LedTypes.hpp"
#include "crosspad/led/ILedStrip.hpp"
#include "crosspad/led/HtmlColor.hpp"

// Platform interfaces
#include "crosspad/platform/IClock.hpp"
#include "crosspad/midi/IMidiOutput.hpp"

// Pad management (portable)
#include "crosspad/pad/PadAnimator.hpp"
#include "crosspad/pad/PadLedController.hpp"
#include "crosspad/pad/PadManager.hpp"

// STM32 manager (portable)
#include "crosspad/stm32/Stm32Manager.hpp"
