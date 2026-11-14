#include "game/coop.h"

#include "game/agents.h"
#include "game/chunkloading.h"
#include "game/controllers.h"
#include "game/colour.h"
#include "game/cutscenereader.h"
#include "game/font.h"
#include "game/objects.h"
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
// The wumpa fruit the second player gathered (the first player's are the game's own count) and its extra lives' worth
u32 g_SecondWumpa = 0;
constexpr u32 WumpaPerLife = 100;
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
        // The game's renderer the first player's half (top, left), the second renderer the second player's
        TakeHalf(first->target, false, g_SideBySide);
        TakeHalf(g_SecondRenderer->target, true, g_SideBySide);
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

    // It roams where it likes: the chunks load around it too (CoopLoadingFocus); only when its chunk is gone (unloaded under it)
    // is it made again beside the first
    if (second->chunk == nullptr)
    {
        TakeAway();
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

    SetRendererView(g_SecondRenderer, g_SecondView);
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

    GameController* controller = G_GameController;
    g_RenderView = &controller->view;
    g_RenderTarget = controller->renderer->target;
    SetUpFrame(controller->renderer->target, 0, 0);
}

void CoopBeforeHud()
{
    if (!g_Split)
    {
        return;
    }

    SetUpFrame(g_Whole, 0, 0);
    g_RenderTarget = g_Whole;
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

void PlaySecond()
{
    GameController* controller = G_GameController;
    CharacterAgent* character = CharacterOf(Second());
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
    if (g_PlayerInstance == g_Second)
    {
        GameController* controller = G_GameController;
        g_PlayerInstance = g_Real.instance;
        if (g_Real.character < GameProgress::Characters && controller->progress.characters[g_Real.character] == g_Second)
        {
            controller->progress.characters[g_Real.character] = g_Real.handle;
        }

        g_PlayerCharacter = g_Real.agent;
        g_PlayerCharacter2 = g_Real.agent2;
        g_PlayerPart = g_Real.part;
        g_PlayerPart2 = g_Real.part2;
    }

    g_ScopeSecond = false;
}

f32 SquaredTo(InstanceContext* from, const Vector4& at)
{
    Vector4 p = PositionOf(from);
    f32 dx = p.x - at.x, dy = p.y - at.y, dz = p.z - at.z;
    return dx * dx + dy * dy + dz * dz;
}
}

CoopPlayerScope::CoopPlayerScope(const void* instance, const void* originator) : saved(nullptr), swapped(false)
{
    InstanceContext* second = Second();
    if (second == nullptr || second->place == nullptr || CharacterOf(second) == nullptr)
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
        if (owner == nullptr || owner->place == nullptr || first->place == nullptr)
        {
            return;
        }

        Vector4 at = PositionOf(owner);
        wantsSecond = SquaredTo(second, at) < SquaredTo(first, at);
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

bool CoopTakesWumpa(InstanceContext* from, u32 count)
{
    InstanceContext* second = Second();
    GameController* controller = G_GameController;
    InstanceContext* first = controller->progress.Instance(controller->progress.play.character);
    if (second == nullptr || from == nullptr || from->place == nullptr)
    {
        return false;
    }

    // The nearer player's: the one the fruit flew to, or that broke the crate
    Vector4 at = PositionOf(from);
    Vector4 b = PositionOf(second);
    f32 dx = b.x - at.x, dy = b.y - at.y, dz = b.z - at.z;
    f32 toSecond = dx * dx + dy * dy + dz * dz;
    if (first != nullptr && first->place != nullptr)
    {
        Vector4 a = PositionOf(first);
        dx = a.x - at.x;
        dy = a.y - at.y;
        dz = a.z - at.z;
        if (dx * dx + dy * dy + dz * dz <= toSecond)
        {
            return false;
        }
    }

    g_SecondWumpa += count;
    while (g_SecondWumpa >= WumpaPerLife)
    {
        g_SecondWumpa -= WumpaPerLife;
    }

    return true;
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

void CoopQueueHud()
{
    if (!g_Split)
    {
        return;
    }

    // The second player's wumpa fruit at its half's top left: the HUD's own wumpa sprite, the count beside it and a small P2
    constexpr f32 IconWidth = 0.06f;
    constexpr f32 IconHeight = 0.08f;
    GameController* controller = G_GameController;
    Renderer* renderer = controller->renderer;
    OLEG* oleg = &controller->oleg;
    f32 left = g_SplitSideBySide ? 0.5f : 0.0f;
    f32 top = g_SplitSideBySide ? 0.0f : 0.5f;
    f32 iconX = left + 0.07f;
    f32 iconY = top + 0.08f;
    u32 colour;
    GetColor(&colour, ColourWhite);

    Sprite* sprite = &oleg->sprites[OLEG::SpriteWumpa];
    if (sprite->material != nullptr && 0.0f < sprite->texture.width && 0.0f < sprite->texture.height)
    {
        Matrix4x4 matrix;
        InitIdentityMatrix(&matrix);
        matrix.m[0][0] = IconWidth / sprite->texture.width;
        matrix.m[1][1] = IconHeight / sprite->texture.height;
        // (a turned sprite is placed by its middle, another by its corner: SpriteWidget::Draw)
        matrix.m[3][0] = sprite->turned != 0 ? iconX : iconX - IconWidth * 0.5f;
        matrix.m[3][1] = sprite->turned != 0 ? iconY : iconY - IconHeight * 0.5f;
        renderer->colour = colour;
        QueuePlacedShape(renderer, &matrix, sprite, oleg->hudWumpaSprite.flags.layer);
    }

    char text[16];
    u32 n = g_SecondWumpa;
    char digits[12];
    s32 count = 0;
    do
    {
        digits[count++] = static_cast<char>('0' + n % 10);
        n /= 10;
    } while (n != 0 && count < 11);

    s32 at = 0;
    while (count > 0)
    {
        text[at++] = digits[--count];
    }

    text[at] = 0;
    renderer->font = &controller->font;
    renderer->colour = colour;
    renderer->textScale.x = 1.0f;
    renderer->textScale.y = 1.0f;
    renderer->textAlignment.value = TextAlignment::MiddleLeft;
    QueueText(renderer, text, iconX + 0.05f, iconY);
    renderer->textScale.x = 0.5f;
    renderer->textScale.y = 0.5f;
    renderer->textAlignment.value = TextAlignment::Centred;
    QueueText(renderer, "P2", iconX, iconY + 0.075f);
}
