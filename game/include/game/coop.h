#pragma once

#include "common.h"

struct GameController;
struct InstanceContext;

// TwinsanityEvolved (naser949's two-player mod): a second player on the second controller. Pressing START on it while the game
// plays makes a second copy of the played character beside the first and gives it that controller (the camera stays the first
// player's). The second player is taken away while a cutscene plays, the first rides a vehicle or the game is not in play, and
// made again beside the first when play goes on. A death of the second player never restarts the checkpoint: it is made again
// beside the first after a moment. When it falls far behind (or below), it is brought back beside the first. Holding SELECT on
// the second controller for a second leaves. Nothing happens until the second controller presses START: a game played alone
// is the game's own.

// Once a frame, from GameController::Update (every state: the second player is taken away outside play)
void StepCoop(GameController* controller);
// Whether the instance is the second player's character (its scripts' checkpoint restart is the second player's death)
bool CoopIsSecondPlayer(InstanceContext* instance);
// The second player's death (its character's death script asked for the checkpoint restart): made again beside the first
void CoopSecondPlayerDied();
// While the second player's character is being made: the game factory makes it a character of its own (GameFactoryStandIn: no
// character already in play stands in for it, and it does not become the progress's character)
bool CoopMakingSecondPlayer();
