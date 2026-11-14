#include "game/coop.h"

#include "game/agents.h"
#include "game/chunkloading.h"
#include "game/controllers.h"
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

namespace
{

// The second controller (the pad controller's second pad: src/main.cpp makes two)
constexpr u32 SecondPadIndex = 1;
// Frames of play before the second player is made (a level's start settles first), and after its death
constexpr u32 SettleFrames = 30;
constexpr u32 DeathFrames = 90;
// Frames SELECT is held to leave
constexpr u32 LeaveFrames = 60;
// Where the second player is made: beside the first and a little above it (it drops to the ground)
constexpr f32 SpawnSide = 0.8f;
constexpr f32 SpawnAbove = 1.0f;
// The second player is brought back beside the first past this distance, or this far below it
constexpr f32 TetherDistance = 20.0f;
constexpr f32 TetherBelow = 12.0f;

// The second player's character (a reference: the game lets it go with its chunk)
Reference* g_Second = nullptr;
// Whether the second player has joined (START), and the frames left before it is made
bool g_Joined = false;
u32 g_Wait = 0;
u32 g_Selecting = 0;
bool g_Making = false;

GamePad* SecondPad()
{
    auto* pads = static_cast<GamePadController*>(G_GamePadController);
    return pads != nullptr ? pads->pads[SecondPadIndex] : nullptr;
}

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
void Warp(InstanceContext* instance, const Vector4& position)
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
        controls->pad = SecondPad();
    }

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

// ---- the split screen ----------------------------------------------------------------------------------------------------
bool g_SideBySide = false;
bool g_Split = false;
bool g_SplitSideBySide = false;
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
    InstanceContext* camera = SecondCamera();
    u32 state = controller->State();
    bool wants = camera != nullptr && (state == GameController::StatePlaying || state == GameController::StatePaused);
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

    if (!g_Split || g_SplitSideBySide != g_SideBySide)
    {
        g_Split = true;
        g_SplitSideBySide = g_SideBySide;
        // The first renderer (drawn first) the second player's half, the second renderer the first player's
        TakeHalf(first->target, true, g_SideBySide);
        TakeHalf(g_SecondRenderer->target, false, g_SideBySide);
    }

    g_SecondRenderer->flags.draws = 1;
    g_SecondRenderer->flags.clearsColour = 1;
    g_SecondRenderer->flags.clearsDepth = 1;
    AssignReference(&g_SecondView->cameraObject, camera);
}

}

void StepPlayers(GameController* controller)
{
    GamePad* pad = SecondPad();
    if (pad == nullptr)
    {
        return;
    }

    InstanceContext* first = FirstInPlay(controller);
    if (!g_Joined)
    {
        if (first != nullptr && GetButtonState(pad, PadStart, true))
        {
            g_Joined = true;
            g_Wait = 0;
        }

        return;
    }

    // Leaving: SELECT held; a tap turns the split screen
    if (GetButtonState(pad, PadSelect, false))
    {
        g_Selecting++;
    }
    else
    {
        if (g_Selecting > 0 && g_Selecting < LeaveFrames)
        {
            g_SideBySide = !g_SideBySide;
        }

        g_Selecting = 0;
    }

    if (g_Selecting >= LeaveFrames)
    {
        g_Joined = false;
        g_Selecting = 0;
        TakeAway();
        StepSplit(controller);
        return;
    }

    InstanceContext* second = Second();
    if (first == nullptr)
    {
        // Out of play (a cutscene, a vehicle, a load, the menus): taken away, made again once play goes on
        if (second != nullptr && controller->State() != GameController::StatePaused)
        {
            TakeAway();
        }

        if (controller->State() != GameController::StatePaused && g_Wait < SettleFrames)
        {
            g_Wait = SettleFrames;
        }

        return;
    }

    if (second == nullptr)
    {
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
        // Its death plays out, then it's made again beside the first
        CoopSecondPlayerDied();
        return;
    }

    // Too far behind, or fallen below: brought back beside the first (made again when it's in another chunk)
    Vector4 a = PositionOf(first);
    Vector4 b = PositionOf(second);
    f32 dx = b.x - a.x;
    f32 dy = b.y - a.y;
    f32 dz = b.z - a.z;
    if (dx * dx + dy * dy + dz * dz > TetherDistance * TetherDistance || dy < -TetherBelow)
    {
        if (second->chunk == first->chunk)
        {
            Warp(second, BesideFirst(first));
        }
        else
        {
            TakeAway();
        }
    }
}

void StepCoop(GameController* controller)
{
    StepPlayers(controller);
    StepSplit(controller);
}

bool CoopIsSecondPlayer(InstanceContext* instance)
{
    return instance != nullptr && instance == Second();
}

void CoopSecondPlayerDied()
{
    TakeAway();
    g_Wait = DeathFrames;
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

    GameController* controller = G_GameController;
    SetRendererView(g_SecondRenderer, &controller->view);
    SetRendererView(controller->renderer, g_SecondView);
}

void CoopBeginScene(Renderer* renderer)
{
    if (!g_Split)
    {
        return;
    }

    g_StampMark = renderer == g_SecondRenderer ? 0 : SecondPassMark;
    SetUpFrame(renderer->target, 0, 0);
}

void CoopEndScene()
{
    if (!g_Split)
    {
        return;
    }

    g_StampMark = 0;
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
    if (!g_Split)
    {
        return;
    }

    g_RenderView = &G_GameController->view;
    g_RenderTarget = g_SecondRenderer->target;
    SetUpFrame(g_SecondRenderer->target, 0, 0);
}

void CoopBeforeHud()
{
    if (!g_Split)
    {
        return;
    }

    SetUpFrame(g_Whole, 0, 0);
    g_RenderTarget = g_Whole;
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
