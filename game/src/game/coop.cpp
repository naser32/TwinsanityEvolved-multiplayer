#include "game/coop.h"

#include "game/agents.h"
#include "game/chunkloading.h"
#include "game/controllers.h"
#include "game/colour.h"
#include "game/cutscenereader.h"
#include "game/font.h"
#include "game/objects.h"
#include "platform/system.h"
#include "game/oleg.h"
#include "game/shapes.h"
#include "game/overlay.h"
#include "game/clock.h"
#include "game/renderer.h"
#include "game/view.h"
#include "game/memory.h"
#include "platform/graphics.h"
#include "game/followcamera.h"
#include "game/gamecontroller.h"
#include "game/instancefactory.h"
#include "game/instances.h"
#include "game/math.h"
#include "game/objectnode.h"
#include "game/pads.h"
#include "game/particles.h"
#include "game/place.h"
#include "game/player.h"
#include "game/progress.h"
#include "game/reference.h"
#include "game/resources.h"

// TwinsanityEvolved: the two-player mod (game/coop.h)

extern "C"
{
    // The chunk manager the instances' chunks are asked of (game/src/game/commandscharacters.cpp's)
    extern void* G_ChunkManager;
}

// (defined with the players' step and the player scope below)
void CoopForgetScopedSecond();
InstanceContext* ScopedRealPrimary();
InstanceContext* Primary(GameController* controller);
bool MenuState(u32 state);

namespace
{

// The second controller (the pad controller's second pad: src/main.cpp makes two)
constexpr u32 SecondPadIndex = 1;
// Frames of play before the second player is made (a level's start settles first), and after its death
constexpr u32 SettleFrames = 30;
// Frames SELECT is held to leave
constexpr u32 LeaveFrames = 60;
// Where the second player is made: beside the first and a little above it (it drops to the ground)
constexpr f32 SpawnSide = 0.8f;
constexpr f32 SpawnAbove = 1.0f;

// The second player's character (a reference: the game lets it go with its chunk)
Reference* g_Second = nullptr;
// Whether the second player has joined (START), and the frames left before it is made
bool g_Joined = false;
u32 g_Wait = 0;
u32 g_Selecting = 0;
bool g_Making = false;

// ---- who plays which character, lives, and who is out ---------------------------------------------------------------------
// The game's own played character is the PRIMARY, the copy the SECONDARY. Player 1 (the first controller, the top or left half)
// plays the primary and player 2 the secondary -- until the primary's player dies while the other lives: the primary is not let
// die (the game would restart its checkpoint); it takes the secondary's place and the SURVIVOR plays it from there (swapped),
// the dead player is out. A player who is out watches the other (its half shows the other's camera) until a checkpoint is
// reached, then comes back as the secondary beside the other -- while it has lives left. When the last character dies, the
// game restarts its checkpoint as it always does, and both come back.
bool g_Swapped = false;
bool g_Down[2] = {false, false};
bool g_PendingSwap = false;
// Frames the secondary has been dead for, and how many its death may take
u32 g_DeadFrames = 0;
constexpr u32 DeathFramesMost = 300;
bool g_Restarted = false;
bool g_GameOver = false;
// Frames the join prompt stays after play starts
constexpr u32 PromptFrames = 600;
u32 g_PromptFrames = 0;

u32 PrimaryPlayer()
{
    return g_Swapped ? 1 : 0;
}

u32 SecondaryPlayer()
{
    return g_Swapped ? 0 : 1;
}

GamePad* PadOfPlayer(u32 player)
{
    auto* pads = static_cast<GamePadController*>(G_GamePadController);
    return pads != nullptr ? pads->pads[player] : nullptr;
}

// The lives and the wumpa fruit are the game's own, shared by the two players: a death of either takes a life
u32 LivesOf(u32)
{
    return G_GameController->progress.counts.lives;
}

void TakeLife(u32)
{
    G_GameController->oleg.AddLives(Platform::System::InfiniteLives() != 0 ? 0 : -1, 0);
}

GamePad* SecondPad()
{
    auto* pads = static_cast<GamePadController*>(G_GamePadController);
    return pads != nullptr ? pads->pads[SecondPadIndex] : nullptr;
}

u32 SecondaryPlayer();
GamePad* PadOfPlayer(u32 player);

InstanceContext* Second()
{
    return g_Second != nullptr ? static_cast<InstanceContext*>(g_Second->object) : nullptr;
}

CharacterAgent* CharacterOf(InstanceContext* instance)
{
    auto* node = instance != nullptr ? static_cast<PlayerNode*>(GetGameNode(&instance->nodes, NodeCharacter)) : nullptr;
    return node != nullptr ? node->character : nullptr;
}

Vector4 PositionOf(InstanceContext* instance)
{
    ObjectPlace* place = instance->place;
    place->SyncPosition();
    return place->position;
}

// Beside the first player: its right hand side, above it
Vector4 BesideFirst(InstanceContext* first)
{
    Vector4 position = PositionOf(first);
    ObjectPlace* place = first->place;
    const f32 (*m)[4] = place->matrix.m;
    position.x = position.x + m[0][0] * SpawnSide;
    position.y = position.y + m[0][1] * SpawnSide + SpawnAbove;
    position.z = position.z + m[0][2] * SpawnSide;
    position.w = 1.0f;
    return position;
}

// Warped as the tests warp the played character (game/src/game/testrequests.cpp Warp: PositionWarp's WarpTo)
[[maybe_unused]] void Warp(InstanceContext* instance, const Vector4& position)
{
    ObjectPlace* place = instance->place;
    place->SyncPosition();
    if (place->MoveTo(&position))
    {
        QueueObject(instance);
    }

    auto* node = static_cast<ObjectNode*>(GetGameNode(&instance->nodes, NodeObject));
    if (node != nullptr)
    {
        node->frameStart = position;
    }
}

// The second player's character let go of (released: gone from its chunk)
void TakeAway()
{
    // (a scope may have it in the played character's place right now -- its own script asked for this: the game's player
    // first, or the game would keep a character that is gone as its player)
    CoopForgetScopedSecond();
    InstanceContext* second = Second();
    if (second != nullptr)
    {
        auto* controls = static_cast<ControlsNode*>(GetGameNode(&second->nodes, NodeControls));
        if (controls != nullptr)
        {
            controls->pad = nullptr;
        }

        CallVirtual<u32>(second, second->vtable, ReferencedObject::ReleaseSlot);
    }

    RemoveReference(&g_Second);
    g_Second = nullptr;
}

// A copy of the first player's character made beside it (as a script's SpawnObject makes a copy of its own object: the source's
// properties, its object), played with the second controller; its follow camera stopped (the view stays the first player's)
InstanceContext* Make(InstanceContext* first)
{
    CharacterAgent* character = CharacterOf(first);
    ChunkEntry* chunk = ChunkOfInstance(G_ChunkManager, first);
    if (character == nullptr || chunk == nullptr)
    {
        return nullptr;
    }

    u32 objectId = character->objectId & ResourceIndexMask;
    Vector4 position = BesideFirst(first);
    s32 angles[3] = {0, 0, 0};
    ObjectPlace* place = first->place;
    place->SyncRotation();
    AnglesOfRotation(&place->rotation, &angles[0], &angles[1], &angles[2]);

    InstanceFactory* factory = g_InstanceFactory;
    factory->SetInstanceProperties();
    factory->ClearUnused1();
    factory->SetGivesIds();
    factory->ClearGivesFlagSlots();
    factory->creationFlags = 0;
    g_Making = true;
    InstanceContext* made = CreateInstanceFrom(factory, chunk, first, objectId, &position, angles, 0);
    g_Making = false;
    if (made == nullptr || made == first || CharacterOf(made) == nullptr)
    {
        return nullptr;
    }

    auto* controls = static_cast<ControlsNode*>(GetGameNode(&made->nodes, NodeControls));
    if (controls != nullptr)
    {
        controls->pad = PadOfPlayer(SecondaryPlayer());
    }

    // The character's model stays the first player's to the cutscenes (CharacterAgent::SetUp gave it to the copy)
    u16 model;
    GetObjectModelId(&model, character->object, 0);
    SetVideoModelInstance(G_VideoController, model & ResourceIndexMask, first);
    // (its follow node keeps following it: FollowNode::Update lets the second player's camera step, the split screen's view)
    AssignReference(&g_Second, made);
    return made;
}

// Whether the first player's character can have a second beside it now: in play, on foot
InstanceContext* FirstInPlay(GameController* controller)
{
    if (controller->State() != GameController::StatePlaying)
    {
        return nullptr;
    }

    InstanceContext* first = controller->progress.Instance(controller->progress.play.character);
    CharacterAgent* character = CharacterOf(first);
    if (character == nullptr || first->chunk == nullptr || character->vehicle != nullptr || character->state.dead != 0)
    {
        return nullptr;
    }

    return first;
}

// ---- the two players' options (the Options menu's page: olegpages.cpp) ----------------------------------------------------
// How much wider than a game played alone a half sees (the camera's "distance": a wider view shows more around the character,
// and draws more), and how far below its middle a top or bottom half looks (a share of the full view's half height)
constexpr f32 CameraWiden[] = {1.0f, 1.3f, 1.6f, 2.0f};
constexpr f32 CameraTilt[] = {0.0f, 0.42f, 0.6f};

f32 Widen()
{
    s32 value = CoopOptionValue(CoopOptionCamera);
    return CameraWiden[value < 0 ? 0 : value > 3 ? 3 : value];
}

f32 TiltShare()
{
    s32 value = CoopOptionValue(CoopOptionTilt);
    return CameraTilt[value < 0 ? 0 : value > 2 ? 2 : value];
}

// ---- online (the platform's netplay: platform/nativeee/host/te_netplay.h) ------------------------------------------------
// Two machines run this same game in step, each showing ONE player's view on its whole screen. The game does on both what a
// split screen does (two renderers, two cameras) with both views the size of the screen, and tells the platform whose view
// the frames that follow are (MarkView); the platform draws its own player's and lets the other's go. Whether a session runs
// is the platform's to say: the game asks it through the mods' page of that name (a row no page shows), every frame.
s32 g_OnlineMod = -2;
bool g_Online = false;
bool g_SplitOnline = false;

bool AskOnline()
{
    char text[32];
    if (g_OnlineMod == -2)
    {
        g_OnlineMod = -1;
        const char* name = "online";
        for (u32 mod = 0; mod < 16 && g_OnlineMod < 0; mod++)
        {
            text[0] = 0;
            Platform::System::ModPageLabel(mod, text, sizeof(text));
            u32 at = 0;
            while (name[at] != 0 && text[at] == name[at])
            {
                at++;
            }

            if (name[at] == 0 && text[at] == 0)
            {
                g_OnlineMod = static_cast<s32>(mod);
            }
        }
    }

    if (g_OnlineMod < 0)
    {
        return false;
    }

    text[0] = 0;
    Platform::System::ModPageRowLabel(static_cast<u32>(g_OnlineMod), 100, text, sizeof(text));
    return text[0] == '1';
}

// Whose view the frames that follow are: 0 nobody's (the whole picture: the HUD, the menus), else 1 + the player
void MarkView(u32 view)
{
    if (!g_SplitOnline)
    {
        return;
    }

    Platform::Graphics::FrameStart mark = {};
    mark.red = 0xC0;
    mark.green = static_cast<u8>(view);
    mark.blue = 0x0F;
    Platform::Graphics::StartFrame(mark);
}

// The game's account of itself after this frame, for the two machines to compare: where both players are, the game's state,
// the lives, the random seed (when two games fall out of step these differ within moments; the host's is then sent again)
void TellState(GameController* controller)
{
    u32 a = 2166136261u;
    u32 b = 0x9E3779B9u;
    auto mix = [&](u32 value)
    {
        a = (a ^ value) * 16777619u;
        b = (b + value) * 0x85EBCA6Bu;
        b ^= b >> 13;
    };
    mix(controller->State());
    mix(LivesOf(0));
    mix(static_cast<u32>(g_RandomSeed));
    mix((g_Swapped ? 1u : 0u) | (g_Joined ? 2u : 0u) | (g_Down[0] ? 4u : 0u) | (g_Down[1] ? 8u : 0u));
    InstanceContext* players[2] = { Primary(controller), Second() };
    for (InstanceContext* player : players)
    {
        auto* node = player != nullptr ? static_cast<ObjectNode*>(GetGameNode(&player->nodes, NodeObject)) : nullptr;
        if (node == nullptr)
        {
            mix(0);
            continue;
        }

        // (the position's three floats, bit for bit)
        const u8* bytes = reinterpret_cast<const u8*>(&node->frameStart);
        for (u32 at = 0; at < 12; at += 4)
        {
            mix(static_cast<u32>(bytes[at]) | static_cast<u32>(bytes[at + 1]) << 8 | static_cast<u32>(bytes[at + 2]) << 16 |
                static_cast<u32>(bytes[at + 3]) << 24);
        }
    }

    Platform::Graphics::FrameStart mark = {};
    mark.x = static_cast<s32>(a);
    mark.y = static_cast<s32>(b);
    mark.red = 0xC1;
    mark.blue = 0x0F;
    Platform::Graphics::StartFrame(mark);
}

// ---- the split screen ----------------------------------------------------------------------------------------------------
bool g_SideBySide = false;
bool g_Split = false;
bool g_SplitSideBySide = false;
bool g_SplitSwapped = false;
bool g_SecondHasCamera = false;
// The whole screen's target (a copy of the first renderer's before the split), the second renderer (made once, kept: the
// renderers' pool has no way out) and the second player's view
RenderTargetDescription* g_Whole = nullptr;
Renderer* g_SecondRenderer = nullptr;
RenderView* g_SecondView = nullptr;
u32 g_StampMark = 0;
constexpr u32 SecondPassMark = 0x8000;

InstanceContext* SecondCamera()
{
    InstanceContext* second = Second();
    auto* follow = second != nullptr ? static_cast<FollowNode*>(GetGameNode(&second->nodes, NodeFollow)) : nullptr;
    auto* camera = follow != nullptr && follow->cameraInstance != nullptr ? static_cast<InstanceContext*>(follow->cameraInstance->object)
                                                                          : nullptr;
    return camera != nullptr && camera->chunk != nullptr ? camera : nullptr;
}

// A target made a part of the whole screen's: the first (top, left) or second (bottom, right) half
void TakeHalf(RenderTargetDescription* target, bool secondHalf, bool sideBySide)
{
    const RenderTargetDescription* whole = g_Whole;
    target->offsetX = whole->offsetX;
    target->offsetY = whole->offsetY;
    target->width = whole->width;
    target->height = whole->height;
    if (sideBySide)
    {
        s32 half = whole->width / 2;
        target->width = secondHalf ? whole->width - half : half;
        target->offsetX = whole->offsetX + (secondHalf ? half : 0);
    }
    else
    {
        s32 half = whole->height / 2;
        target->height = secondHalf ? whole->height - half : half;
        target->offsetY = whole->offsetY + (secondHalf ? half : 0);
    }

    Platform::Graphics::SetUpRenderTarget(target);
}

void TakeWhole(RenderTargetDescription* target)
{
    const RenderTargetDescription* whole = g_Whole;
    target->offsetX = whole->offsetX;
    target->offsetY = whole->offsetY;
    target->width = whole->width;
    target->height = whole->height;
    Platform::Graphics::SetUpRenderTarget(target);
}

void EndSplit(GameController* controller)
{
    if (!g_Split)
    {
        return;
    }

    g_Split = false;
    g_SplitOnline = false;
    TakeWhole(controller->renderer->target);
    if (g_SecondRenderer != nullptr)
    {
        g_SecondRenderer->flags.draws = 0;
        g_SecondRenderer->view = nullptr;
    }

    if (g_SecondView != nullptr)
    {
        RemoveReference(&g_SecondView->cameraObject);
        g_SecondView->cameraObject = nullptr;
    }
}

// The split screen begun, turned or ended with the second player's camera
void StepSplit(GameController* controller)
{
    g_SideBySide = CoopOptionValue(CoopOptionSplit) == 1;
    InstanceContext* camera = SecondCamera();
    u32 state = controller->State();
    // (with a player out the screen is whole again: it watches the other's game, at a game played alone's cost)
    bool wants = camera != nullptr && (state == GameController::StatePlaying || MenuState(state));
    if (!wants)
    {
        EndSplit(controller);
        return;
    }

    Renderer* first = controller->renderer;
    if (g_Whole == nullptr)
    {
        g_Whole = RenderTargetDescription::Copy(
            static_cast<RenderTargetDescription*>(MemoryAllocate(sizeof(RenderTargetDescription))), first->target);
    }
    else if (!g_Split)
    {
        // The whole screen as it is now (a TV setting may have moved it)
        g_Whole->offsetX = first->target->offsetX;
        g_Whole->offsetY = first->target->offsetY;
        g_Whole->width = first->target->width;
        g_Whole->height = first->target->height;
        g_Whole->displayWidth = first->target->displayWidth;
        g_Whole->displayHeight = first->target->displayHeight;
        Platform::Graphics::SetUpRenderTarget(g_Whole);
    }

    if (g_SecondRenderer == nullptr)
    {
        g_SecondRenderer = Renderer::Construct(static_cast<Renderer*>(MemoryAllocate(sizeof(Renderer))), first->controller, g_Whole);
        RendererPoolAdd(&first->controller->renderers, &g_SecondRenderer);
        g_SecondView = RenderView::Construct(static_cast<RenderView*>(MemoryAllocate(sizeof(RenderView))));
    }

    if (!g_Split || g_SplitSideBySide != g_SideBySide || g_SplitSwapped != g_Swapped || g_SplitOnline != g_Online)
    {
        g_Split = true;
        g_SplitSideBySide = g_SideBySide;
        g_SplitSwapped = g_Swapped;
        g_SplitOnline = g_Online;
        if (g_Online)
        {
            // Online: both views the size of the screen (each machine shows one)
            TakeWhole(first->target);
            TakeWhole(g_SecondRenderer->target);
        }
        else
        {
            // The game's renderer draws the primary's player's half (player 1: top, left), the second renderer the other's
            TakeHalf(first->target, g_Swapped, g_SideBySide);
            TakeHalf(g_SecondRenderer->target, !g_Swapped, g_SideBySide);
        }
    }

    g_SecondRenderer->flags.draws = 1;
    g_SecondRenderer->flags.clearsColour = 1;
    g_SecondRenderer->flags.clearsDepth = 1;
    g_SecondHasCamera = camera != nullptr;
    if (camera != nullptr)
    {
        AssignReference(&g_SecondView->cameraObject, camera);
    }
}

}

InstanceContext* Primary(GameController* controller)
{
    // (while a scope has the secondary in the played character's place -- its own frame, its scripts -- the real one)
    if (InstanceContext* real = ScopedRealPrimary())
    {
        return real;
    }

    return controller->progress.Instance(controller->progress.play.character);
}

// The secondary died: its player is out
void SecondaryDied()
{
    if (Second() == nullptr)
    {
        return;
    }

    u32 player = SecondaryPlayer();
    TakeLife(player);
    g_Down[player] = true;
    g_DeadFrames = 0;
    TakeAway();
}

// An instance put at a place in a chunk (its follow camera with it, starting over)
void PutAt(InstanceContext* instance, const Matrix4x4* matrix, ChunkData* chunk)
{
    auto* follow = static_cast<FollowNode*>(GetGameNode(&instance->nodes, NodeFollow));
    if (chunk != nullptr && chunk != instance->chunk)
    {
        MoveToChunk(chunk, instance);
        ReferencedObject* camera = follow != nullptr && follow->cameraInstance != nullptr ? follow->cameraInstance->object : nullptr;
        if (camera != nullptr)
        {
            MoveToChunk(chunk, camera);
        }
    }

    if (SetPlaceMatrix(instance->place, matrix) != 0)
    {
        QueueObject(instance);
    }

    auto* node = static_cast<ObjectNode*>(GetGameNode(&instance->nodes, NodeObject));
    if (node != nullptr)
    {
        Vector4 position = *RowOf(matrix, 3);
        position.w = 1.0f;
        node->frameStart = position;
    }

    if (follow != nullptr)
    {
        follow->camera.rig.Restart();
    }
}

// The primary was about to die while the secondary lives. The game's played character must not die (the game would restart its
// checkpoint), so the two change places: the primary goes on where the survivor stood, played by the survivor, and the
// secondary dies where the primary was, in the dead player's half -- its death plays out there, then its player is out
void ChangePlaces(InstanceContext* first, InstanceContext* second)
{
    RotateAndTranslate(first->place);
    RotateAndTranslate(second->place);
    Matrix4x4 dying = first->place->matrix;
    Matrix4x4 living = second->place->matrix;
    ChunkData* dyingChunk = first->chunk;
    ChunkData* livingChunk = second->chunk;
    CharacterAgent* character = CharacterOf(first);
    CharacterAgent* other = CharacterOf(second);
    u32 points = static_cast<CreaturePart*>(other->part)->flags.hitPoints;
    static_cast<CreaturePart*>(character->part)->flags.hitPoints = points != 0 ? points : 1;
    other->velocity = character->velocity;
    character->velocity = {0.0f, 0.0f, 0.0f, 1.0f};
    PutAt(first, &living, livingChunk);
    PutAt(second, &dying, dyingChunk);
    g_Swapped = !g_Swapped;
    auto* controls = static_cast<ControlsNode*>(GetGameNode(&second->nodes, NodeControls));
    if (controls != nullptr)
    {
        controls->pad = PadOfPlayer(SecondaryPlayer());
    }

    g_DeadFrames = 0;
    static_cast<CreaturePart*>(other->part)->flags.hitPoints = 0;
    CoopKillCharacter(other);
}

bool MenuState(u32 state)
{
    return state == GameController::StatePaused || state == GameController::StateGallery || state == GameController::StateWaiting;
}

bool RestartState(u32 state)
{
    // (not the fades and starts a cutscene goes through: a player who is out stays out past a cutscene; the game's own
    // checkpoint restart says so itself: CoopPrimaryRestart)
    return state == GameController::StateLoadingLevel || state == GameController::StateNewGame ||
           state == GameController::StateGameOver || state == GameController::StateMainMenu ||
           state == GameController::StateTitle;
}

void StepPlayers(GameController* controller)
{
    GamePad* pad = SecondPad();
    if (pad == nullptr)
    {
        return;
    }

    u32 state = controller->State();
    if (state == GameController::StateGameOver)
    {
        g_GameOver = true;
    }

    if (RestartState(state))
    {
        g_Restarted = true;
    }
    else if (state == GameController::StatePlaying && g_Restarted)
    {
        // Play goes on after a restart or a load: player 1 plays the primary again, and whoever has lives is back
        g_Restarted = false;
        g_Swapped = false;
        g_PendingSwap = false;
        g_GameOver = false;
        g_Down[0] = false;
        g_Down[1] = false;
        g_Wait = SettleFrames;
        g_PromptFrames = g_Joined ? 0 : PromptFrames;
    }

    if (state == GameController::StatePlaying)
    {
        if (g_PromptFrames > 0)
        {
            g_PromptFrames--;
        }
    }

    // The menus over the game (pause, options): everyone stays as they are
    if (MenuState(state))
    {
        return;
    }

    InstanceContext* first = FirstInPlay(controller);
    if (!g_Joined)
    {
        if (first != nullptr && GetButtonState(pad, PadStart, true))
        {
            g_Joined = true;
            g_Swapped = false;
            g_Down[0] = false;
            g_Down[1] = false;
            g_Wait = 0;
            g_PromptFrames = 0;
        }

        return;
    }

    // Leaving: SELECT held on the second controller (the first controller plays the primary again)
    g_Selecting = GetButtonState(pad, PadSelect, false) ? g_Selecting + 1 : 0;
    if (g_Selecting >= LeaveFrames)
    {
        g_Joined = false;
        g_Selecting = 0;
        g_Swapped = false;
        g_PendingSwap = false;
        g_Down[0] = false;
        g_Down[1] = false;
        TakeAway();
        if (first != nullptr)
        {
            auto* controls = static_cast<ControlsNode*>(GetGameNode(&first->nodes, NodeControls));
            if (controls != nullptr && controls->pad != nullptr)
            {
                controls->pad = PadOfPlayer(0);
            }
        }

        StepSplit(controller);
        return;
    }

    InstanceContext* second = Second();
    if (first == nullptr)
    {
        // Out of play (a cutscene, a vehicle, a load): the secondary taken away, made again once play goes on
        g_PendingSwap = false;
        if (second != nullptr)
        {
            TakeAway();
        }

        if (g_Wait < SettleFrames)
        {
            g_Wait = SettleFrames;
        }

        return;
    }

    // The primary is played by its player's controller (the game gives it the first one whenever it hands control back)
    auto* controls = static_cast<ControlsNode*>(GetGameNode(&first->nodes, NodeControls));
    if (controls != nullptr && controls->pad != nullptr)
    {
        controls->pad = PadOfPlayer(PrimaryPlayer());
    }

    if (g_PendingSwap)
    {
        g_PendingSwap = false;
        CharacterAgent* survivor = CharacterOf(second);
        if (second != nullptr && survivor != nullptr && survivor->state.dead == 0 && second->chunk != nullptr)
        {
            ChangePlaces(first, second);
            return;
        }
    }

    if (second == nullptr)
    {
        // Out: waiting for a checkpoint (CoopCheckpointReached)
        if (g_Down[SecondaryPlayer()])
        {
            return;
        }

        if (g_Wait > 0)
        {
            g_Wait--;
            return;
        }

        Make(first);
        return;
    }

    CharacterAgent* character = CharacterOf(second);
    if (character == nullptr)
    {
        TakeAway();
        return;
    }

    if (character->state.dead != 0)
    {
        // Its death plays out (its script ends it: CoopSecondPlayerDied); out after a while at the latest, or with its chunk
        if (++g_DeadFrames > DeathFramesMost || second->chunk == nullptr)
        {
            SecondaryDied();
        }

        return;
    }

    g_DeadFrames = 0;
    // It roams where it likes: the chunks load around it too (CoopLoadingFocus); only when its chunk is gone (unloaded under it)
    // is it made again beside the primary
    if (second->chunk == nullptr)
    {
        TakeAway();
    }
}

void StepCoop(GameController* controller)
{
    CoopUntiltCameras();
    g_Online = AskOnline();
    StepPlayers(controller);
    StepSplit(controller);
    if (g_Online)
    {
        TellState(controller);
    }
}

bool CoopIsSecondPlayer(InstanceContext* instance)
{
    return instance != nullptr && instance == Second();
}

void CoopSecondPlayerDied()
{
    // Its own script is running: it is out at the next frame (StepPlayers), not under its script's feet
    g_DeadFrames = DeathFramesMost;
}

bool CoopPrimaryDying(const void* agent)
{
    InstanceContext* second = Second();
    CharacterAgent* survivor = CharacterOf(second);
    if (!g_Joined || survivor == nullptr || survivor->state.dead != 0 || second->chunk == nullptr)
    {
        return false;
    }

    GameController* controller = G_GameController;
    if (controller->State() != GameController::StatePlaying)
    {
        return false;
    }

    CharacterAgent* character = CharacterOf(Primary(controller));
    if (character == nullptr || character != agent)
    {
        return false;
    }

    // Not dead: it takes the secondary's place at the next frame (StepPlayers), with at least a hit point until then
    auto* part = static_cast<CreaturePart*>(character->part);
    if (part->flags.hitPoints == 0)
    {
        part->flags.hitPoints = 1;
    }

    g_PendingSwap = true;
    return true;
}

bool CoopPrimaryRestart()
{
    if (!g_Joined)
    {
        return false;
    }

    // The secondary lives: no restart, the primary takes its place
    InstanceContext* second = Second();
    CharacterAgent* survivor = CharacterOf(second);
    GameController* controller = G_GameController;
    if (survivor != nullptr && survivor->state.dead == 0 && second->chunk != nullptr)
    {
        CharacterAgent* character = CharacterOf(Primary(controller));
        if (character != nullptr)
        {
            character->state.dead = 0;
            character->buttons.locked.value = 0;
            auto* part = static_cast<CreaturePart*>(character->part);
            if (part->flags.hitPoints == 0)
            {
                part->flags.hitPoints = 1;
            }
        }

        g_PendingSwap = true;
        return true;
    }

    // The last character died: the game restarts its checkpoint (or ends, with no lives left) as it always does, and both
    // players are back after it
    g_Restarted = true;
    return false;
}

bool CoopTakesLife(InstanceContext* owner)
{
    if (!g_Joined || owner == nullptr)
    {
        return false;
    }

    // The secondary's own death scripts take nothing (SecondaryDied did); the primary's take its player's life
    if (owner == Second())
    {
        return true;
    }

    return false;
}

void CoopCheckpointReached()
{
    for (u32 player = 0; player < 2; player++)
    {
        if (g_Joined && g_Down[player] && LivesOf(player) > 0)
        {
            g_Down[player] = false;
            g_Wait = 20;
        }
    }
}

bool CoopMakingSecondPlayer()
{
    return g_Making;
}

bool CoopSplit()
{
    return g_Split;
}

void CoopSetViews()
{
    if (!g_Split)
    {
        return;
    }

    // The secondary's camera; with its player out, the primary's (the game's own view)
    SetRendererView(g_SecondRenderer, g_SecondHasCamera ? g_SecondView : &G_GameController->view);
}

bool CoopDrawScenes()
{
    if (!g_Split)
    {
        return false;
    }

    // The second player's half first, the first player's last: the chunks' camera matrices the frame leaves (the controls',
    // the particles') are the first player's, and the game's renderer keeps the first player's view as in a game played alone
    DrawRendererScene(g_SecondRenderer);
    // The particles in the second player's half too (the chunks' camera matrices are its half's now): drawn only, the frame's
    // update and the first player's draw follow as in a game played alone (UpdateAndDrawParticles)
    if (g_ParticlesSetUp != 0)
    {
        g_RenderView = g_SecondRenderer->view;
        g_RenderTarget = g_SecondRenderer->target;
        MarkView(1 + SecondaryPlayer());
        SetUpFrame(g_SecondRenderer->target, 0, 0);
        Platform::Graphics::UseHelperPrograms(Platform::Graphics::CullingPrograms, false);
        ParticleViews views;
        views.frame = g_RenderedFrames;
        views.count = 0;
        DrawParticleList(0, 0, &views);
        DrawParticleList(0, DrawListDistortion, &views);
        for (s32 index = 0; index < views.count; index++)
        {
            views.chunks[index]->particleView = -1;
        }
    }

    DrawRendererScene(G_GameController->renderer);
    return true;
}

void CoopBeginScene(Renderer* renderer)
{
    if (!g_Split)
    {
        return;
    }

    g_StampMark = renderer == g_SecondRenderer ? SecondPassMark : 0;
    MarkView(1 + (renderer == g_SecondRenderer ? SecondaryPlayer() : 1 - SecondaryPlayer()));
    SetUpFrame(renderer->target, 0, 0);
}

void CoopEndScene()
{
    if (!g_Split)
    {
        return;
    }

    g_StampMark = 0;
    MarkView(0);
    SetUpFrame(g_Whole, 0, 0);
    g_RenderTarget = g_Whole;
}

u32 CoopDrawStampMark()
{
    return g_StampMark;
}

f32 CoopViewShape(const RenderTargetDescription* target)
{
    if (!g_Split || target == g_Whole)
    {
        return 1.0f;
    }

    const RenderTargetDescription* whole = g_Whole;
    return (static_cast<f32>(target->width) * static_cast<f32>(whole->height)) /
           (static_cast<f32>(target->height) * static_cast<f32>(whole->width));
}

void CoopAfterScenes()
{
    CoopUntiltCameras();
    if (!g_Split)
    {
        return;
    }

    GameController* controller = G_GameController;
    g_RenderView = &controller->view;
    g_RenderTarget = controller->renderer->target;
    // (what follows the scenes is the game's own renderer's: the primary's player's view)
    MarkView(1 + (1 - SecondaryPlayer()));
    SetUpFrame(controller->renderer->target, 0, 0);
}

void CoopBeforeHud()
{
    if (g_Split)
    {
        MarkView(0);
        SetUpFrame(g_Whole, 0, 0);
        g_RenderTarget = g_Whole;
    }

    CoopQueueHud();
}

const Matrix4x4* CoopCameraMatrix(InstanceContext* instance)
{
    if (!g_Split || instance == nullptr || instance != Second())
    {
        return nullptr;
    }

    InstanceContext* camera = SecondCamera();
    if (camera == nullptr)
    {
        return nullptr;
    }

    ObjectPlace* place = camera->place;
    RotateAndTranslate(place);
    return &place->matrix;
}

// ---- the second player as "the player" ---------------------------------------------------------------------------------
namespace
{
// The game's own player (the first), kept while a scope has the second player in its place, and how deep the scopes are
struct RealPlayer
{
    Reference* instance;
    Reference* handle;
    u32 character;
    PlayerCharacter* agent;
    PlayerCharacter* agent2;
    CharacterPart* part;
    CharacterPart* part2;
};
RealPlayer g_Real;
u32 g_ScopeDepth = 0;
bool g_ScopeSecond = false;

// The reference a scope put in the played character's place (the secondary's, as it was then)
Reference* g_Placed = nullptr;

void PlaySecond()
{
    GameController* controller = G_GameController;
    CharacterAgent* character = CharacterOf(Second());
    g_Placed = g_Second;
    g_PlayerInstance = g_Second;
    if (g_Real.character < GameProgress::Characters)
    {
        controller->progress.characters[g_Real.character] = g_Second;
    }

    auto* agent = reinterpret_cast<PlayerCharacter*>(character);
    auto* part = static_cast<CharacterPart*>(character->part);
    g_PlayerCharacter = agent;
    g_PlayerCharacter2 = agent;
    g_PlayerPart = part;
    g_PlayerPart2 = part;
    g_ScopeSecond = true;
}

void PlayFirst()
{
    // (a character switch made inside keeps what it set)
    if (g_PlayerInstance == g_Placed)
    {
        GameController* controller = G_GameController;
        g_PlayerInstance = g_Real.instance;
        if (g_Real.character < GameProgress::Characters && controller->progress.characters[g_Real.character] == g_Placed)
        {
            controller->progress.characters[g_Real.character] = g_Real.handle;
        }

        g_PlayerCharacter = g_Real.agent;
        g_PlayerCharacter2 = g_Real.agent2;
        g_PlayerPart = g_Real.part;
        g_PlayerPart2 = g_Real.part2;
    }

    g_Placed = nullptr;
    g_ScopeSecond = false;
}

f32 SquaredBetween(const Vector4& p, const Vector4& at)
{
    f32 dx = p.x - at.x, dy = p.y - at.y, dz = p.z - at.z;
    return dx * dx + dy * dy + dz * dz;
}

struct PlayerPlaces
{
    u32 frame;
    const InstanceContext* first;
    const InstanceContext* second;
    bool valid;
    Vector4 a;
    Vector4 b;
};
PlayerPlaces g_Places = {~0u, nullptr, nullptr, false, {}, {}};
}

void CoopForgetScopedSecond()
{
    if (g_ScopeDepth != 0 && g_ScopeSecond)
    {
        PlayFirst();
    }
}

InstanceContext* ScopedRealPrimary()
{
    return g_ScopeDepth != 0 && g_ScopeSecond && g_Real.instance != nullptr ? static_cast<InstanceContext*>(g_Real.instance->object)
                                                                            : nullptr;
}

CoopPlayerScope::CoopPlayerScope(const void* instance, const void* originator) : saved(nullptr), swapped(false)
{
    InstanceContext* second = Second();
    if (second == nullptr)
    {
        return;
    }

    GameController* controller = G_GameController;
    Reference* firstHandle = g_ScopeDepth != 0 ? g_Real.instance : g_PlayerInstance;
    auto* first = firstHandle != nullptr ? static_cast<InstanceContext*>(firstHandle->object) : nullptr;
    if (first == nullptr || first == second)
    {
        return;
    }

    // The two players' places, taken once a frame (thousands of scripts ask)
    if (g_Places.frame != g_RenderedFrames || g_Places.first != first || g_Places.second != second)
    {
        g_Places.frame = g_RenderedFrames;
        g_Places.first = first;
        g_Places.second = second;
        g_Places.valid = second->place != nullptr && first->place != nullptr && CharacterOf(second) != nullptr;
        if (g_Places.valid)
        {
            g_Places.a = PositionOf(first);
            g_Places.b = PositionOf(second);
        }
    }

    if (!g_Places.valid)
    {
        return;
    }

    // Whose it is: the second player's own (or started by it), the first player's own, else the nearer player's
    bool wantsSecond;
    if (instance == second || originator == second)
    {
        wantsSecond = true;
    }
    else if (instance == first || originator == first)
    {
        wantsSecond = false;
    }
    else
    {
        auto* owner = static_cast<InstanceContext*>(const_cast<void*>(instance));
        // (only what a player plays against: a cutscene's director, a trigger or a camera near the second player still means
        // the game's own played character by "the player")
        constexpr u32 NearestKinds = 1u << NodeCrate | 1u << NodePickup | 1u << NodeCreature | 1u << NodeProjectile;
        if (owner == nullptr || owner->place == nullptr || (owner->nodes.mask & NearestKinds) == 0)
        {
            return;
        }

        Vector4 at = PositionOf(owner);
        wantsSecond = SquaredBetween(g_Places.b, at) < SquaredBetween(g_Places.a, at);
    }

    if (g_ScopeDepth == 0)
    {
        if (!wantsSecond)
        {
            return;
        }

        u32 character = controller->progress.play.character;
        g_Real.instance = g_PlayerInstance;
        g_Real.character = character;
        g_Real.handle = character < GameProgress::Characters ? controller->progress.characters[character] : nullptr;
        g_Real.agent = g_PlayerCharacter;
        g_Real.agent2 = g_PlayerCharacter2;
        g_Real.part = g_PlayerPart;
        g_Real.part2 = g_PlayerPart2;
    }

    swapped = true;
    saved = g_ScopeSecond ? g_Second : nullptr;
    g_ScopeDepth++;
    if (wantsSecond != g_ScopeSecond)
    {
        if (wantsSecond)
        {
            PlaySecond();
        }
        else
        {
            PlayFirst();
        }
    }
}

CoopPlayerScope::~CoopPlayerScope()
{
    if (!swapped)
    {
        return;
    }

    g_ScopeDepth--;
    bool wasSecond = saved != nullptr && g_ScopeDepth != 0;
    if (wasSecond != g_ScopeSecond)
    {
        if (wasSecond && Second() != nullptr && CharacterOf(Second()) != nullptr)
        {
            PlaySecond();
        }
        else
        {
            PlayFirst();
        }
    }
}

// ---- how near an instance was seen: the nearer of the two halves ---------------------------------------------------------
namespace
{
struct SeenEntry
{
    const InstanceContext* instance;
    u32 distance;
    u32 frame;
};
constexpr u32 SeenSlots = 4096;
SeenEntry g_Seen[SeenSlots];

SeenEntry* SeenSlot(const InstanceContext* instance, bool make)
{
    u32 frame = g_RenderedFrames;
    u32 at = (reinterpret_cast<u32>(instance) >> 4) & (SeenSlots - 1);
    for (u32 probe = 0; probe < 16; probe++)
    {
        SeenEntry* entry = &g_Seen[(at + probe) & (SeenSlots - 1)];
        if (entry->frame == frame && entry->instance == instance)
        {
            return entry;
        }

        if (entry->frame != frame)
        {
            if (!make)
            {
                return nullptr;
            }

            entry->instance = instance;
            entry->frame = frame;
            entry->distance = ~0u;
            return entry;
        }
    }

    return nullptr;
}
}

u32 CoopSeen(InstanceContext* instance, u32 distance)
{
    if (!g_Split)
    {
        return distance;
    }

    if (g_StampMark != 0)
    {
        // The second player's half (drawn first): kept for the first player's
        SeenEntry* entry = SeenSlot(instance, true);
        if (entry != nullptr && distance < entry->distance)
        {
            entry->distance = distance;
        }

        return distance;
    }

    SeenEntry* entry = SeenSlot(instance, false);
    return entry != nullptr && entry->distance < distance ? entry->distance : distance;
}

void CoopSkinMatrices(const GameOGI* ogi, Matrix4x4** matrices)
{
    if (!CoopKeepsDrawn())
    {
        return;
    }

    // The skin's draw multiplies the joints' matrices in place: the first half's draw works on a copy, the second on them
    auto* copy = static_cast<Matrix4x4*>(Platform::Graphics::AllocFrameMemory(ogi->jointCount, sizeof(Matrix4x4)));
    if (copy == nullptr)
    {
        return;
    }

    for (u32 index = 0; index < ogi->jointCount; index++)
    {
        copy[index] = (*matrices)[index];
    }

    *matrices = copy;
}

bool CoopTakesWumpa(InstanceContext*, u32)
{
    // The wumpa fruit are the game's own count, whoever picks them up
    return false;
}

ReferencedObject* CoopLoadingFocus()
{
    InstanceContext* second = Second();
    return second != nullptr && second->chunk != nullptr ? second : nullptr;
}

bool CoopKeepsDrawn()
{
    // The second player's half is drawn first (its pass's mark set): what it draws is drawn again in the first player's half
    return g_Split && g_StampMark != 0;
}

namespace
{
void Text(Renderer* renderer, const char* text, f32 x, f32 y, f32 scale, u32 alignment)
{
    renderer->textScale.x = scale;
    renderer->textScale.y = scale;
    renderer->textAlignment.value = alignment;
    QueueText(renderer, text, x, y);
}
}

void CoopQueueHud()
{
    // (not over the pause menu and its pages: as the first player's counts)
    GameController* controller = G_GameController;
    if (controller->State() != GameController::StatePlaying)
    {
        return;
    }

    Renderer* renderer = controller->renderer;
    u32 colour;
    GetColor(&colour, ColourWhite);
    renderer->font = &controller->font;
    renderer->colour = colour;
    if (!g_Joined)
    {
        // The second controller's way in, for a while after play starts
        if (g_PromptFrames > 0)
        {
            Text(renderer, "player 2  press start to join", 0.5f, 0.93f, 0.5f, TextAlignment::Centred);
        }

        return;
    }

    // A player who is out watches the other's game: who, and until when
    for (u32 player = 0; player < 2; player++)
    {
        if (g_Down[player] && Second() == nullptr)
        {
            const char* why = LivesOf(player) > 0 ? "is out  back at the next checkpoint" : "is out of lives";
            Text(renderer, player == 0 ? "player 1" : "player 2", 0.5f, 0.86f, 0.6f, TextAlignment::Centred);
            Text(renderer, why, 0.5f, 0.92f, 0.5f, TextAlignment::Centred);
        }
    }

    // (online each screen is one player's: no half to name)
    if (!g_Split || g_SplitOnline)
    {
        return;
    }

    // P2 in the second player's half's corner
    f32 left = g_SplitSideBySide ? 0.5f : 0.0f;
    f32 top = g_SplitSideBySide ? 0.0f : 0.5f;
    Text(renderer, "P2", left + 0.05f, top + 0.05f, 0.5f, TextAlignment::Centred);
}

u32 CoopInDrawnCell(InstanceContext* instance, u32 inView)
{
    // The first player's half (drawn second): an instance the second player's half had in view stays in a drawn cell (its
    // model is made and animated next frame), whatever the first player's view says
    if (!g_Split || g_StampMark != 0 || inView != 0)
    {
        return inView;
    }

    return SeenSlot(instance, false) != nullptr && instance->flags.inDrawnCell != 0 ? 1 : 0;
}

void CoopNearerListener(const Vector4* at, Vector4* local)
{
    if (!g_Split)
    {
        return;
    }

    InstanceContext* camera = SecondCamera();
    if (camera == nullptr || camera->place == nullptr)
    {
        return;
    }

    // The point in the second player's camera's space (the chunks taken as one space): heard from the nearer of the two
    ObjectPlace* place = camera->place;
    RotateAndTranslate(place);
    Matrix4x4 view = place->matrix;
    VuInvertRigidInPlace(&view);
    Vector4 other;
    VuTransformPoint(&view, at, &other);
    if (other.x * other.x + other.y * other.y + other.z * other.z < local->x * local->x + local->y * local->y + local->z * local->z)
    {
        *local = other;
    }
}

// ---- a half's view: the first player's width of view, less height ---------------------------------------------------------
// A top or bottom half is twice as wide as it is high: keeping the game's vertical field of view there would show twice the
// width of a game played alone in each half (and build twice its geometry, twice over). The half keeps the usual width of view
// and sees half the height instead: things are the size they are in a game played alone. A left or right half keeps the
// height and sees half the width.

namespace
{
// What a half's vertical field of view (its tangent) is of the lens's: a top or bottom half's the wider view's over its shape,
// a left or right half's the wider view's
f32 VerticalShare(f32 shape)
{
    return 1.0f < shape ? Widen() / shape : shape < 1.0f ? Widen() : 1.0f;
}
}

void CoopFixProjection(const RenderTargetDescription* target, RenderView* view)
{
    // (never split: the projection is the game's own, untouched)
    InstanceContext* camera = view->cameraObject != nullptr ? static_cast<InstanceContext*>(view->cameraObject->object) : nullptr;
    auto* lens = camera != nullptr ? static_cast<CameraLensNode*>(GetGameNode(&camera->nodes, NodeCameraLens)) : nullptr;
    if (g_Whole == nullptr || lens == nullptr)
    {
        return;
    }

    // The projection made from the lens each time (RenderView::SetProjection's, with the half's shape): whatever the view kept
    f32 shape = CoopViewShape(target);
    f32 vertical = VerticalShare(shape);
    s32 half = static_cast<s32>(static_cast<f32>(lens->fov) * 0.5f);
    f32 cotangent = 1.0f / TanOfAngle(&half);
    f32 inverseAspect = 1.0f / (lens->pixelAspect * TvAspect() * shape);
    view->projection.m[1][1] = cotangent / vertical;
    view->projection.m[0][0] = inverseAspect * cotangent / vertical;
}

s32 CoopViewFov(const RenderTargetDescription* target, s32 fov)
{
    f32 vertical = VerticalShare(CoopViewShape(target));
    if (g_Whole == nullptr || vertical == 1.0f)
    {
        return fov;
    }

    // The angle whose half's tangent is the lens's half's times the share (the tangent grows with the angle: halved in)
    s32 half = static_cast<s32>(static_cast<f32>(fov) * 0.5f);
    f32 wanted = TanOfAngle(&half) * vertical;
    s32 low = 0;
    s32 high = half * 2;
    for (s32 step = 0; step < 24 && low + 1 < high; step++)
    {
        s32 middle = low + (high - low) / 2;
        f32 tangent = TanOfAngle(&middle);
        if (0.0f < tangent && tangent < wanted)
        {
            low = middle;
        }
        else
        {
            high = middle;
        }
    }

    return high * 2;
}

// ---- a top or bottom half's camera looks a little lower -------------------------------------------------------------------
// The half sees half the height of a full view, around its middle; the played character stands below the middle of the game's
// view (the half showed its head at the bottom edge). While the half's scene is drawn its camera is tilted down, so the
// character is in the half's middle; the camera's own matrix is put back after the scenes (nothing else sees the tilt).
namespace
{
struct TiltedCamera
{
    ObjectPlace* place;
    Vector4 up;
    Vector4 forward;
};
TiltedCamera g_Tilted[2] = {};

void Untilt(TiltedCamera* tilted)
{
    if (tilted->place != nullptr)
    {
        *RowOf(&tilted->place->matrix, 1) = tilted->up;
        *RowOf(&tilted->place->matrix, 2) = tilted->forward;
        tilted->place = nullptr;
    }
}
}

void CoopUntiltCameras()
{
    Untilt(&g_Tilted[0]);
    Untilt(&g_Tilted[1]);
}

void CoopTiltCamera(const RenderTargetDescription* target, RenderView* view)
{
    InstanceContext* camera = view->cameraObject != nullptr ? static_cast<InstanceContext*>(view->cameraObject->object) : nullptr;
    ObjectPlace* place = camera != nullptr ? camera->place : nullptr;
    if (place == nullptr)
    {
        return;
    }

    // (tilted already this frame: from its own matrix again)
    for (TiltedCamera& tilted : g_Tilted)
    {
        if (tilted.place == place)
        {
            Untilt(&tilted);
        }
    }

    auto* lens = static_cast<CameraLensNode*>(GetGameNode(&camera->nodes, NodeCameraLens));
    if (!(1.0f < CoopViewShape(target)) || lens == nullptr)
    {
        return;
    }

    TiltedCamera* slot = g_Tilted[0].place == nullptr ? &g_Tilted[0] : g_Tilted[1].place == nullptr ? &g_Tilted[1] : nullptr;
    if (slot == nullptr)
    {
        return;
    }

    RotateAndTranslate(place);
    s32 half = static_cast<s32>(static_cast<f32>(lens->fov) * 0.5f);
    f32 tangent = TiltShare() * TanOfAngle(&half);
    if (tangent == 0.0f)
    {
        return;
    }

    f32 cosine = 1.0f / __builtin_sqrtf(1.0f + tangent * tangent);
    f32 sine = tangent * cosine;
    Vector4* up = RowOf(&place->matrix, 1);
    Vector4* forward = RowOf(&place->matrix, 2);
    slot->place = place;
    slot->up = *up;
    slot->forward = *forward;
    Vector4 u = *up;
    Vector4 f = *forward;
    // Down: the forward toward what is under it, the up following
    forward->x = f.x * cosine - u.x * sine;
    forward->y = f.y * cosine - u.y * sine;
    forward->z = f.z * cosine - u.z * sine;
    up->x = u.x * cosine + f.x * sine;
    up->y = u.y * cosine + f.y * sine;
    up->z = u.z * cosine + f.z * sine;
}
