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

// ---- The split screen: each player's half of the screen, seen through that player's own follow camera ----------------------
// While the second player is in play the screen is split in two (top and bottom; a tap of SELECT on the second controller
// turns it into left and right and back). The game's renderer keeps the first player's view and draws its half, last; a second
// renderer draws the second player's half first, so the chunks' camera matrices the frame leaves (the controls' and the
// particles') are the first player's, as in a game played alone. The platform holds each half's draws to its half (StartFrame's frame smaller than the
// screen). The HUD and the menus are drawn over the whole screen.

struct Renderer;
struct RenderTargetDescription;
struct Matrix4x4;

// Whether the screen is split now
bool CoopSplit();
// After the game controller set its view (GameContext::GameEndFrame): the second renderer given the second player's view
void CoopSetViews();
// The split screen's scenes drawn (the second player's half, then the first player's): false when the screen isn't split (the
// renderers draw theirs)
bool CoopDrawScenes();
// A renderer's scene starts (DrawRendererScene: its half's frame set up) and ends (the whole screen's again, for the overlay)
void CoopBeginScene(Renderer* renderer);
void CoopEndScene();
// The chunks' draw stamp's mark for the scene being drawn: a chunk is drawn once a frame per player's half
u32 CoopDrawStampMark();
// The aspect a renderer's target has over the whole screen's (1 when the screen isn't split)
f32 CoopViewShape(const RenderTargetDescription* target);
// After the scenes (the particles and decals go in the first player's half), and before the HUD (the whole screen)
void CoopAfterScenes();
void CoopBeforeHud();
// The camera the second player's stick is turned by (its own follow camera's matrix while the screen is split; else none: the
// chunk's, the first player's camera)
const Matrix4x4* CoopCameraMatrix(InstanceContext* instance);

// ---- The second player as "the player" -------------------------------------------------------------------------------------
// While the second player's character steps (its agent's frame, its own scripts), a script started by it runs, or the script
// of anything nearer to it than to the first player runs (a creature it landed on, an enemy beside it), the game's player
// (g_PlayerInstance, the progress's played character, g_PlayerCharacter: the scripts' "player") is the second player: the
// creature bounces it, the enemies go for it, the pickups fly to it. The first player's own scripts always see the first.
struct Reference;
struct ReferencedObject;
class CoopPlayerScope
{
public:
    CoopPlayerScope(const void* instance, const void* originator);
    ~CoopPlayerScope();
    CoopPlayerScope(const CoopPlayerScope&) = delete;
    CoopPlayerScope& operator=(const CoopPlayerScope&) = delete;

private:
    Reference* saved;
    bool swapped;
};

// Wumpa fruit picked up at an instance (a pickup, a crate): the second player's when it is the nearer player (true: counted on
// its own count, shown in its half), else the game's
bool CoopTakesWumpa(InstanceContext* from, u32 count);
// The chunks load around the second player's character too (none: only around the first's)
ReferencedObject* CoopLoadingFocus();
// The second player's count queued over its half (before the HUD is drawn)
void CoopQueueHud();
// While the second player's half is being drawn (first): a model drawn stays to be drawn again in the first player's half
// (ModelNode::Draw forgets what it drew: retail draws a model once a frame)
bool CoopKeepsDrawn();
// The distance an instance is seen at (scenery.cpp SetSeen): on a split screen the nearer of the two halves', so what the second
// player is near animates at its full rate however far the first player is
u32 CoopSeen(InstanceContext* instance, u32 distance);
// Before a skin's joints are multiplied in place for its draw (DrawOgi): a copy for the first of the two halves' draws
struct GameOGI;
void CoopSkinMatrices(const GameOGI* ogi, Matrix4x4** matrices);
// Whether an instance out of the first player's view stays in a drawn cell: it does when the second player's half saw it
u32 CoopInDrawnCell(InstanceContext* instance, u32 inView);
// A sound's place in the listener's space: the second player's camera's instead when the sound is nearer to it (at: the
// sound's place in the world)
struct Vector4;
void CoopNearerListener(const Vector4* at, Vector4* local);
// A half's view (see coop.cpp): what the view's aspect is multiplied by, the projection's height set after the view's
// update, and the field of view its frustum is made for (with the aspect times CoopViewShape)
struct RenderView;
f32 CoopViewAspect(const RenderTargetDescription* target);
void CoopFixProjection(const RenderTargetDescription* target, RenderView* view);
s32 CoopViewFov(const RenderTargetDescription* target, s32 fov);
