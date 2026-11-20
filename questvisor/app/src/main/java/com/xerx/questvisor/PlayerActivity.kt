// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Xerx. Part of QuestVisor; see LICENSE (GPL-3.0 with an additional permission
// to combine with the Meta Spatial SDK).
package com.xerx.questvisor

import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Bundle
import android.util.Log
import androidx.compose.ui.platform.ComposeView
import com.meta.spatial.compose.ComposeFeature
import com.meta.spatial.compose.ComposeViewPanelRegistration
import com.meta.spatial.core.Color4
import com.meta.spatial.core.Entity
import com.meta.spatial.core.Pose
import com.meta.spatial.core.Quaternion
import com.meta.spatial.core.SpatialFeature
import com.meta.spatial.core.Vector3
import com.meta.spatial.runtime.EnvironmentDepthMode
import com.meta.spatial.toolkit.AppSystemActivity
import com.meta.spatial.toolkit.Box
import com.meta.spatial.toolkit.DpDisplayOptions
import com.meta.spatial.toolkit.Grabbable
import com.meta.spatial.toolkit.GrabbableType
import com.meta.spatial.toolkit.Material
import com.meta.spatial.toolkit.Mesh
import com.meta.spatial.toolkit.MediaPanelRenderOptions
import com.meta.spatial.toolkit.MediaPanelSettings
import com.meta.spatial.toolkit.Panel
import com.meta.spatial.toolkit.PanelRegistration
import com.meta.spatial.toolkit.PanelRenderMode
import com.meta.spatial.toolkit.PanelStyleOptions
import com.meta.spatial.toolkit.PixelDisplayOptions
import com.meta.spatial.toolkit.QuadShapeOptions
import com.meta.spatial.toolkit.ReadableMediaPanelRenderOptions
import com.meta.spatial.toolkit.ReadableMediaPanelSettings
import com.meta.spatial.toolkit.ReadableVideoSurfacePanelRegistration
import com.meta.spatial.toolkit.Transform
import com.meta.spatial.toolkit.TransformParent
import com.meta.spatial.toolkit.UIPanelSettings
import com.meta.spatial.toolkit.VideoSurfacePanelRegistration
import com.meta.spatial.vr.VRFeature
import java.lang.reflect.Method

class PlayerActivity : AppSystemActivity() {
    companion object {
        const val TAG = "QuestVisor"
        private const val USE_SCENE = "com.oculus.permission.USE_SCENE"
        private const val REQ_USE_SCENE = 4711

        @Volatile
        var current: PlayerActivity? = null
    }

    private var videoPanel: Entity? = null
    private var videoFrame: List<Entity> = emptyList()
    private var controlPanel: Entity? = null

    // Occlusion mode: the video is drawn as an in-scene mesh (readable surface) so Meta's environment depth can hide
    // it behind real objects such as hands. The default is the direct compositor layer, which has lower latency but
    // is composited over everything and cannot be occluded.
    var occlusionEnabled = false
        private set
    // Panel (and frame) being replaced by a mode switch; destroyed once the new panel's surface reached the decoder,
    // so the decoder never renders into a surface that is already gone.
    private var retiringVideo: List<Entity> = emptyList()

    // Screen geometry, all user-configurable (control panel + launch extras, saved in prefs):
    //  - screenWidth: width in metres (arc length when curved), screenDistance: eye to screen centre,
    //  - screenCurve: 0 = flat, 1 = cylinder centred on the head (radius = distance), in between = larger radius,
    //  - fitDeg: preset that wraps the screen around the head so the image's diagonal spans this angle; overrides
    //    width and curve while on. 155 (lensFovDeg) = the O4 Pro / Avata 2 lens, natural angles; 100 = the whole
    //    image inside the Quest's field of view. 0 = off.
    //  - head lock caps the screen at headLockFovDeg (the Quest sees ~100 deg; a wider head-locked screen has edges
    //    you can never look at).
    // EXPERIMENTAL, DOES NOT WORK YET (launch extra --ez softedges true only, no button; see BUILD_AND_STATUS.md
    // "Soft occlusion edges"). Idea: draw the occlusion panel with an own shader (src/shaders/softOcclusion*) that
    // fades the video over a few depth texels instead of the SDK's hard cut. Result on the headset: the shader's
    // `depth` texture only contains virtual geometry (the controller model), never the room/hands, in both
    // TEXTURE_ONLY and OCCLUSION mode. depthReverseZ picks the shader's depth convention (reverse-Z confirmed by the
    // controller occluding the right way round).
    var softEdges = false
        private set
    private var depthReverseZ = true
    // Headset test 22:59: with EnvironmentDepthMode.TEXTURE_ONLY the shader's `depth` texture held only virtual
    // geometry (the controller model occluded, hands and real objects did not), so the room depth reaches it only
    // through the SDK's OCCLUSION pass, which writes environment depth into the scene depth. Soft edges therefore run
    // in OCCLUSION mode with the video material's own depth test off (no hard cut); --es softdepth texture restores
    // the TEXTURE_ONLY variant for comparison.
    private var softUsesOcclusionPass = true

    var screenWidth = 1.6f
        private set
    var screenCurve = 0f
        private set
    var fitDeg = 0f
        private set
    private val headLockFovDeg = 100f
    private var screenDistance = 1.5f
    private var lensFovDeg = 155f
    // Head lock: the video screen follows the headset every frame (position and full orientation, like the goggles'
    // own screen) instead of staying where it was placed. The control panel stays put in the room.
    var headLocked = false
        private set

    // Late system, every frame after the viewer pose is updated:
    //  - head lock: re-places the screen from the head pose (the Settings button is a child of the screen root);
    //  - Settings button: visible while a controller or hand points at the screen (or at the button under it),
    //    for 1.5 s after, and always while no video is decoding;
    //  - two-handed scaling: both controllers squeezing (grip) or both hands grabbing/pinching, both pointing at the
    //    screen -> the screen scales live with the distance between them; on release the size is kept.
    private inner class ScreenSystem : com.meta.spatial.core.SystemBase() {
        override fun execute() {
            if (headLocked) {
                placeVideoAtHead(levelOnly = false)
            }
            try {
                updateScreenInteraction()
            } catch (t: Throwable) {
                if (!interactionErrorLogged) {
                    interactionErrorLogged = true
                    Log.w(TAG, "screen interaction failed: $t")
                }
            }
        }
    }

    private var interactionErrorLogged = false
    private val controllerQuery by lazy { com.meta.spatial.core.Query.where { has(com.meta.spatial.toolkit.Controller.id) } }
    private var lastPointedAtMs = 0L
    private var buttonShown = true
    private var frameCounter = 0
    private var lastDecoded = -1L
    private var lastDecodeChangeMs = 0L
    private var scaling = false
    private var lastScaleHintMs = 0L
    private var scaleAnchorPose: Pose? = null
    // The SDK grab only provides the movement (FACE, which auto-turns the screen towards the user, was not wanted).
    private val screenGrabType = GrabbableType.PIVOT_Y
    // One-hand move: the SDK grab moves the screen; its yaw and pitch follow the attitude of the controller / hand
    // holding it (applied in updateScreenInteraction, after the grab system, so it overrides PIVOT_Y's rotation).
    private var moveGrabberId: Long? = null
    private var moveStartForward: Vector3? = null
    private var moveStartYaw: Quaternion = Quaternion(1f, 0f, 0f, 0f)
    private var moveStartPitch = 0f
    // Tilt layer between the root (moved by the SDK grab, which rewrites the root's transform every frame while
    // held) and the screen content. Its local rotation = extra yaw about the root's vertical, then pitch about the
    // resulting horizontal axis: never roll. Reset by a recenter, kept across panel rebuilds.
    private var videoTilt: Entity? = null
    private var tiltYaw: Quaternion = Quaternion(1f, 0f, 0f, 0f)   // identity: the constructor is (w, x, y, z); (0, 0, 0, 1) is a 180 deg roll
    private var tiltPitch = 0f
    // Size set by two-handed scaling, kept as a uniform Scale on the screen root (saved). Applying it instead of
    // rebuilding the panel avoids the flash and dropped frames a rebuild causes on release. Explicit size changes
    // (Size buttons, presets) fold it back into the screen width and reset it to 1.
    private var screenScale = 1f
    private val handReleaseGraceMs = 200L
    private val lastGrabMs = HashMap<Long, Long>()
    private val lastPointerPose = HashMap<Long, Pose>()
    private var scaleStartDistance = 1f
    private var scaleFactor = 1f
    private val loggedButtonStates = HashSet<String>()
    // Grab surface extents in the root's local space (set by createGrabRoot): half width, half height, plane z.
    private var grabHalfW = 0f
    private var grabHalfH = 0f
    private var grabPlaneZ = 0f

    private fun updateScreenInteraction() {
        val root = videoRoot ?: return
        val rootPose = root.getComponent<Transform>().transform
        val now = android.os.SystemClock.uptimeMillis()
        val bits = com.meta.spatial.runtime.ButtonBits

        // Active controllers / hands: pointing pose and whether they are grabbing. Hand pinches are debounced: the
        // Spatial SDK only reports the pinch as a button bit (no pinch strength, so no distance hysteresis as in
        // Meta's Interaction SDK), and ML hand tracking releases the pinch whenever tracking blinks. A hand counts
        // as still grabbing for handReleaseGraceMs after its bit drops, at its last tracked pose if it went inactive.
        data class Pointer(val id: Long, val pose: Pose, val grabbing: Boolean, val hitsScreen: Boolean)
        val pointers = ArrayList<Pointer>(4)
        for (e in controllerQuery.eval()) {
            val c = e.getComponent<com.meta.spatial.toolkit.Controller>()
            val id = e.id
            val isHand = c.type == com.meta.spatial.toolkit.ControllerType.HAND
            if (!c.isActive) {
                val held = lastGrabMs[id]
                val last = lastPointerPose[id]
                if (isHand && held != null && last != null && now - held < handReleaseGraceMs) {
                    pointers += Pointer(id, last, true, rayHitsScreen(rootPose, last))
                }
                continue
            }
            val pose = e.getComponent<Transform>().transform
            lastPointerPose[id] = pose
            val state = c.buttonState
            // Controllers grab with the grip (squeeze). Hand tracking reports an index pinch as ButtonA (right hand,
            // 0x1) / ButtonX (left, 0x4) (logged on the headset 23:4x), a middle pinch probably as B / Y; trigger and
            // squeeze are kept for other recognisers.
            val grabMask = if (isHand) bits.ButtonA or bits.ButtonB or bits.ButtonX or bits.ButtonY or
                    bits.ButtonSqueezeL or bits.ButtonSqueezeR or bits.ButtonTriggerL or bits.ButtonTriggerR
                else bits.ButtonSqueezeL or bits.ButtonSqueezeR
            val key = "${c.type}:$state"
            if (state != 0 && loggedButtonStates.size < 40 && loggedButtonStates.add(key)) {
                Log.i(TAG, "input: ${c.type} buttons 0x%x".format(state))
            }
            val rawGrab = (state and grabMask) != 0
            if (rawGrab) lastGrabMs[id] = now
            val grab = rawGrab || (isHand && (lastGrabMs[id]?.let { now - it < handReleaseGraceMs } ?: false))
            pointers += Pointer(id, pose, grab, rayHitsScreen(rootPose, pose))
        }

        // Settings button visibility.
        if (pointers.any { it.hitsScreen }) lastPointedAtMs = now
        if (++frameCounter % 15 == 0) {
            // stats[5] = decoded frames in the last second.
            val fps = try { NativePlayer.stats().getOrElse(5) { 0L } } catch (_: Throwable) { 0L }
            if (fps > 0L) lastDecodeChangeMs = now
            if (frameCounter % 60 == 0) updateDisplayRate(fps)   // about once a second
            lastDecoded = fps
        }
        val noVideo = now - lastDecodeChangeMs > 1500
        // While the settings panel is open the button stays, so the settings can always be closed again.
        val show = noVideo || now - lastPointedAtMs < 1500 || scaling || settingsVisible
        if (show != buttonShown) {
            buttonShown = show
            menuButton?.setComponent(com.meta.spatial.toolkit.Visible(show))
        }

        val grabbing = pointers.filter { it.grabbing }

        // One-hand move: while holding, turning the wrist left/right yaws the screen and tilting it up/down pitches
        // it (live, on the tilt layer; the SDK grab keeps moving the root). Yaw = the heading change of the pointing
        // direction about the vertical; pitch = its elevation change. Never roll. Not under head lock or scaling.
        val holder = moveGrabberId?.let { id -> grabbing.firstOrNull { it.id == id } }
        if (scaling || headLocked) {
            moveGrabberId = null
        } else if (holder != null) {
            val f0 = moveStartForward
            if (f0 != null) {
                val f = holder.pose.forward().normalize()
                val f0Flat = Vector3(f0.x, 0f, f0.z)
                val fFlat = Vector3(f.x, 0f, f.z)
                if (f0Flat.length() > 0.1f && fFlat.length() > 0.1f) {
                    tiltYaw = (Quaternion.fromTwoVectors(f0Flat.normalize(), fFlat.normalize()) * moveStartYaw).normalize()
                }
                val dPitch = kotlin.math.asin(f.y.coerceIn(-1f, 1f)) - kotlin.math.asin(f0.y.coerceIn(-1f, 1f))
                tiltPitch = (moveStartPitch + dPitch).coerceIn(-1.3f, 1.3f)
                applyTilt()
            }
        } else if (grabbing.size == 1 && grabbing[0].hitsScreen) {
            moveGrabberId = grabbing[0].id
            moveStartForward = grabbing[0].pose.forward().normalize()
            moveStartYaw = tiltYaw
            moveStartPitch = tiltPitch
        } else if (grabbing.isEmpty()) {
            moveGrabberId = null
        }

        // Two-handed scaling.
        if (!scaling) {
            if (grabbing.size >= 2 && grabbing.none { it.hitsScreen } && now - lastScaleHintMs > 2000) {
                lastScaleHintMs = now
                Log.i(TAG, "scale: two grabs but neither points at the screen")
            }
            // Both grabbing and at least one pointing at the screen (a hand's pointing pose is not always aimed
            // exactly where the user looks, so requiring both missed hand pinches).
            if (grabbing.size >= 2 && grabbing.any { it.hitsScreen }) {
                scaling = true
                scaleStartDistance = (grabbing[0].pose.t + grabbing[1].pose.t * -1f).length().coerceAtLeast(0.05f)
                scaleFactor = 1f
                // Anchor: freeze the screen where it is and switch the one-hand grab off, so scaling only resizes
                // (around the screen's own centre) and never drags the screen along.
                scaleAnchorPose = rootPose
                root.setComponent(Grabbable(false, screenGrabType))
                // Settings out of the way while scaling, and they stay closed afterwards (reopen with the button).
                if (settingsVisible) setSettingsVisible(false)
                Log.i(TAG, "scale: started, hands %.2f m apart".format(scaleStartDistance))
            }
        } else if (grabbing.size >= 2) {
            val d = (grabbing[0].pose.t + grabbing[1].pose.t * -1f).length()
            val target = (d / scaleStartDistance).coerceIn(0.25f, 4f)
            scaleFactor += (target - scaleFactor) * 0.5f          // light smoothing against hand jitter
            scaleAnchorPose?.let { root.setComponent(Transform(it)) }
            val s = screenScale * scaleFactor
            root.setComponent(com.meta.spatial.toolkit.Scale(Vector3(s, s, s)))
        } else {
            scaling = false
            scaleAnchorPose?.let { root.setComponent(Transform(it)) }
            scaleAnchorPose = null
            root.setComponent(Grabbable(true, screenGrabType))
            commitScale(scaleFactor)
        }
    }

    // Display refresh rate follows the stream's frame rate, so every video frame is shown for the same number of
    // refreshes (60 fps on 90 Hz alternates 1 and 2 refreshes = judder; at 100 fps on 90 Hz ~10 % of frames were
    // never shown). The stream rate is the decoder's fps (stats[5]) rounded to 10, accepted after 3 equal readings.
    // Candidates: multiples of it among the Quest 3 rates (lowest first, at least 90 Hz), then 120 and 90; the
    // first one requestExactDisplayRate accepts wins. Nothing changes while no video is decoding.
    private val displayRates = floatArrayOf(72f, 80f, 90f, 100f, 120f)
    private var fpsCandidate = 0
    private var fpsCandidateCount = 0
    private var displayRateForFps = 0
    private var appliedDisplayRate = 0f

    private fun updateDisplayRate(decodedFps: Long) {
        if (decodedFps < 20) return
        val fps = (((decodedFps + 5) / 10) * 10).toInt()
        if (fps != fpsCandidate) {
            fpsCandidate = fps
            fpsCandidateCount = 1
            return
        }
        if (++fpsCandidateCount < 3 || fps == displayRateForFps) return
        displayRateForFps = fps
        val multiples = displayRates.filter { it >= 90f && it.toInt() % fps == 0 }
        val candidates = (multiples + listOf(120f, 90f)).distinct()
        for (rate in candidates) {
            val ok = try { scene.requestExactDisplayRate(rate) } catch (t: Throwable) {
                Log.w(TAG, "display rate: requestExactDisplayRate($rate) threw $t"); false
            }
            Log.i(TAG, "display rate: stream ~$fps fps, request $rate Hz -> ${if (ok) "accepted" else "refused"}")
            if (ok) {
                appliedDisplayRate = rate
                window.decorView.postDelayed({
                    val confirmed = try { scene.getConfirmedFrameRate() } catch (_: Throwable) { -1f }
                    Log.i(TAG, "display rate: confirmed $confirmed Hz (requested $rate for ~$fps fps)")
                }, 1500)
                return
            }
        }
    }

    // True when the pointer's forward ray crosses the grab surface (extended down to include the Settings button).
    private fun rayHitsScreen(rootPose: Pose, pointer: Pose): Boolean {
        if (grabHalfW <= 0f) return false
        val inv = rootPose.inverse()
        val o = inv.times(pointer.t)
        val d = inv.q.times(pointer.forward())
        if (kotlin.math.abs(d.z) < 1e-4f) return false
        val s = screenScale * (scaleFactor.takeIf { scaling } ?: 1f)
        val t = (grabPlaneZ * s - o.z) / d.z
        if (t <= 0f) return false
        val x = (o.x + d.x * t) / s
        val y = (o.y + d.y * t) / s
        // The cog is on the screen itself; when the settings panel is open, its area under the screen counts too.
        val below = if (settingsVisible) 0.05f + settingsPanelHeight / screenScale else 0.02f
        // The cog sits just outside the right edge: extend the area to the right so aiming at it keeps it visible.
        val right = grabHalfW + 0.015f + menuButtonWidth + 0.03f
        return x >= -grabHalfW && x <= right && y <= grabHalfH && y >= -grabHalfH - below
    }

    // Applies a finished two-handed scale by keeping it as the root's scale: no panel rebuild, so nothing flashes.
    private fun commitScale(factor: Float) {
        scaleFactor = 1f
        screenScale = (screenScale * factor).coerceIn(0.1f, 10f)
        applyScreenScale()
        getSharedPreferences("questvisor", MODE_PRIVATE).edit().putFloat("screenScale", screenScale).apply()
        Log.i(TAG, "scale: released at x%.2f -> screen scale %.2f (%.2f m wide)".format(
            factor, screenScale, screenGeometry().width * screenScale))
    }

    // Puts the saved hand scale on the root and re-attaches the Settings button and panel so they keep their own
    // size and stay just under the scaled screen.
    private fun applyScreenScale() {
        videoRoot?.setComponent(com.meta.spatial.toolkit.Scale(Vector3(screenScale, screenScale, screenScale)))
        attachMenuButton()
    }

    // Size buttons: step the visible width (hand scale included) and fold the hand scale back into the width.
    fun stepScreenSize(factor: Float) {
        runOnUiThread {
            val visibleWidth = screenGeometry().width * screenScale
            resetScreenScale()
            setScreen(width = visibleWidth * factor)
        }
    }

    private fun resetScreenScale() {
        if (screenScale == 1f) return
        screenScale = 1f
        getSharedPreferences("questvisor", MODE_PRIVATE).edit().putFloat("screenScale", 1f).apply()
    }

    // Settings panel visibility, toggled by the small button under the screen (saved).
    var settingsVisible = true
        private set
    private var menuButton: Entity? = null
    private var controlFrame: List<Entity> = emptyList()
    // Settings cog: a small square panel in the screen's lower-right corner.
    private val menuButtonWidth = 0.07f
    private val menuButtonHeight = 0.07f
    private val menuButtonDistance = 1.0f
    // Stream aspect ratio the current panel was built for; the panel is rebuilt when the stream's changes
    // (16:9 normal modes, 4:3 race mode).
    private var panelAspect = 16f / 9f
    private val aspectWatch = object : Runnable {
        override fun run() {
            checkStreamAspect()
            window.decorView.postDelayed(this, 1000)
        }
    }

    private val usbHelper = UsbGoggles()

    private var h265Decoder: String = ""
    private var h264Decoder: String = ""

    private var fakeAssetsLoaded = false
    private var fakeIdrRegex = Regex("black_(\\d+)x(\\d+)")

    private var surfaceReceived = false
    private var pendingConnectRequested = false
    private var pendingSource = "network"
    private var pendingHost = ""
    private var pendingPort = 0
    private var pendingSelftestPath: String? = null
    private var pendingDecoderMode = "auto"
    private var pendingHwExp = ""

    // Network connection state, shared with the control panel. userStopped: the user pressed Disconnect, so
    // discovery must not reconnect by itself until they press Connect again.
    @Volatile var currentHost = ""
        private set
    @Volatile var currentPort = 0
        private set
    @Volatile var userStopped = false
    @Volatile var replaying = false
        private set
    var replaySeconds = 300
        private set
    private var connectingSinceMs = 0L
    private val startedAtMs = android.os.SystemClock.elapsedRealtime()
    private val autoConnectHandler = android.os.Handler(android.os.Looper.getMainLooper())
    private val autoConnectTick = object : Runnable {
        override fun run() {
            autoConnect()
            autoConnectHandler.postDelayed(this, 1000)
        }
    }

    private var vrFeature: VRFeature? = null

    private val panelRegistrationList: List<PanelRegistration> by lazy {
        listOf(
            videoPanelRegistration(),
            occludedVideoPanelRegistration(),
            controlPanelRegistration(),
            menuButtonRegistration(),
        )
    }

    override fun registerFeatures(): List<SpatialFeature> {
        val vr = VRFeature(this)
        vrFeature = vr
        return listOf(
            vr,
            ComposeFeature(),
        )
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        current = this

        val prefs = getSharedPreferences("questvisor", MODE_PRIVATE)
        pendingDecoderMode = normalizeDecoderMode(prefs.getString("decoder", "auto"))
        pendingHwExp = prefs.getString("hwexp", "") ?: ""
        occlusionEnabled = prefs.getBoolean("occlusion", false)
        softEdges = false   // experimental and not working: only enabled per launch (--ez softedges true), never saved
        prefs.edit().remove("softEdges").apply()
        depthReverseZ = prefs.getBoolean("depthReverseZ", true)
        screenWidth = prefs.getFloat("screenWidth", 1.6f)
        screenCurve = prefs.getFloat("screenCurve", 0f)
        // "lensMatched" (bool) was the earlier form of the 155-degree preset.
        fitDeg = prefs.getFloat("fitDeg", if (prefs.getBoolean("lensMatched", false)) 155f else 0f)
        screenDistance = prefs.getFloat("screenDistance", 1.5f)
        lensFovDeg = prefs.getFloat("fov", 155f)
        headLocked = prefs.getBoolean("headLocked", false)
        screenScale = prefs.getFloat("screenScale", 1f).coerceIn(0.1f, 10f)
        settingsVisible = prefs.getBoolean("settingsVisible", true)
        systemManager.registerLateSystem(ScreenSystem())

        loadFakeIdrAssets()

        h265Decoder = DecoderPicker.pickDecoder("video/hevc")
        h264Decoder = DecoderPicker.pickDecoder("video/avc")

        Log.i(TAG, "H.265 decoder: $h265Decoder")
        Log.i(TAG, "H.264 decoder: $h264Decoder")
        Log.i(TAG, "decoder mode: $pendingDecoderMode hwexp: $pendingHwExp")

        applyReplayBuffer(prefs.getInt("replaySeconds", 300))
        RelayDiscovery.start(this)
        autoConnectHandler.postDelayed(autoConnectTick, 1000)

        handleIntent(intent)
    }

    override fun onNewIntent(intent: Intent?) {
        super.onNewIntent(intent)
        if (intent != null) {
            handleIntent(intent)
        }
    }

    override fun onResume() {
        super.onResume()
        usbHelper.register(this)
    }

    override fun onPause() {
        super.onPause()
        usbHelper.unregister()
    }

    override fun onSceneReady() {
        super.onSceneReady()
        scene.enablePassthrough(true)

        createVideoPanel(occlusionEnabled, Pose(Vector3(0.0f, 1.6f, 1.5f)))

        // Not grabbable: it hangs under the Settings button as a child of the screen root (attachMenuButton).
        controlPanel = Entity.create(
            Panel(R.id.control_panel),
            Transform(Pose(Vector3(0.0f, 1.05f, 1.4f))),
        )

        controlPanel?.let {
            controlFrame = createFrame(it, 1.1f, 0.45f, 0.012f, 0.018f)
            it.setComponent(com.meta.spatial.toolkit.Visible(settingsVisible))
            controlFrame.forEach { box -> box.setComponent(com.meta.spatial.toolkit.Visible(settingsVisible)) }
        }
        menuButton = Entity.create(Panel(R.id.menu_button), Transform(Pose(Vector3(0f, 1.2f, 1.0f))))
        attachMenuButton()

        applyEnvironmentDepth()
        recenterPanelsNowAndSoon()
        tryStartPending()
        window.decorView.postDelayed(aspectWatch, 1000)

        Log.i(TAG, "scene ready; passthrough enabled; occlusion ${if (occlusionEnabled) "on" else "off"}; " +
            "screen ${describeScreen()}")
    }

    // Horizontal and vertical angle (radians) of an image whose diagonal spans diagDeg, split by aspect ratio.
    // For the lens this assumes an equidistant fisheye (angle proportional to image distance): 155 deg gives
    // ~135 x 76 deg at 16:9 and ~124 x 93 deg at 4:3 (race mode); 100 deg gives ~87 x 49 and ~80 x 60.
    private fun diagonalFov(aspect: Float, diagDeg: Float): Pair<Float, Float> {
        val diag = Math.toRadians(diagDeg.toDouble()).toFloat()
        val d = kotlin.math.sqrt(aspect * aspect + 1f)
        return Pair(diag * aspect / d, diag / d)
    }

    // Fit angle in effect: the preset, capped at headLockFovDeg while head-locked; 0 = no preset.
    private fun effectiveFitDeg(): Float {
        if (fitDeg <= 0f) return 0f
        val deg = if (fitDeg >= 150f) lensFovDeg else fitDeg
        return if (headLocked) kotlin.math.min(deg, headLockFovDeg) else deg
    }

    // Effective geometry: width, height (m), curve (0..1) and cylinder radius (m, 0 = flat).
    private data class ScreenGeometry(val width: Float, val height: Float, val curve: Float, val radius: Float)

    private fun screenGeometry(): ScreenGeometry {
        val fit = effectiveFitDeg()
        if (fit > 0f) {
            // Cylinder centred on the head: the arc spans the horizontal angle exactly. A cylinder is straight
            // vertically, so the height is the arc length of the vertical angle: angle-correct at the image centre,
            // slightly compressed towards the top and bottom edges.
            val (h, v) = diagonalFov(panelAspect, fit)
            return ScreenGeometry(screenDistance * h, screenDistance * v, 1f, screenDistance)
        }
        val curve = screenCurve.coerceIn(0f, 1f)
        val radius = if (curve < 0.01f) 0f else screenDistance / curve
        var width = screenWidth
        if (headLocked) {
            // Keep a head-locked screen inside the Quest's view: horizontal span at most that of a 100-deg diagonal.
            val hCap = diagonalFov(panelAspect, headLockFovDeg).first
            val maxWidth = if (curve >= 0.99f) screenDistance * hCap
                else 2f * screenDistance * kotlin.math.tan(hCap / 2f)
            width = kotlin.math.min(width, maxWidth)
        }
        return ScreenGeometry(width, width / panelAspect, curve, radius)
    }

    private fun describeScreen(): String {
        val g = screenGeometry()
        val arcDeg = if (g.radius > 0f) Math.toDegrees((g.width / g.radius).toDouble()) else 0.0
        val fit = effectiveFitDeg()
        return "%s%s %.2f x %.2f m, curve %d %%%s, distance %.1f m".format(
            if (fit > 0f) "fit %.0f deg".format(fit) else if (g.radius > 0f) "curved" else "flat",
            if (headLocked) ", head-locked" else "",
            g.width, g.height, (g.curve * 100).toInt(), if (g.radius > 0f) " (r %.1f m, arc %.0f deg)".format(g.radius, arcDeg) else "",
            screenDistance)
    }

    private fun videoShape(): com.meta.spatial.toolkit.MediaPanelShapeOptions {
        val g = screenGeometry()
        return if (g.radius > 0f) {
            com.meta.spatial.toolkit.CylinderShapeOptions(g.radius, g.width, g.height)
        } else {
            QuadShapeOptions(g.width, g.height)
        }
    }

    // The screen is two entities. The ROOT carries the pose, the Grabbable and an invisible box mesh just behind
    // the screen that is the grab target; the video PANEL is its child (identity local pose) and is not hittable,
    // so rays pass through it to the box. All placement moves the root. History: a Grabbable on the panel never
    // grabbed (panel surface hit first, then with NoCollision nothing hittable on the Grabbable entity itself).
    private var videoRoot: Entity? = null
    private var showGrabSurface = false   // --ez showgrab true: tint the grab box to see where it is

    private fun createVideoPanel(occluded: Boolean, pose: Pose) {
        val g = screenGeometry()
        val id = if (occluded) R.id.video_panel_occluded else R.id.video_panel
        val root = createGrabRoot(pose, g)
        // root (SDK-grabbed) -> tilt (yaw/pitch from the holding wrist) -> video panel, cog, settings panel.
        val tilt = Entity.create(Transform(Pose(Vector3(0f, 0f, 0f))), TransformParent(root))
        val panel = Entity.create(
            Panel(id, com.meta.spatial.toolkit.MeshCollision.NoCollision),
            // Curved: the panel entity is the cylinder centre, one radius back towards the viewer from the root.
            Transform(Pose(Vector3(0f, 0f, if (g.radius > 0f) -g.radius else 0f))),
            TransformParent(tilt),
        )
        videoRoot = root
        videoTilt = tilt
        videoPanel = panel
        videoFrame = listOf(tilt, root)
        applyTilt()
        applyScreenScale()    // saved hand scale on the root, then attach the Settings button and panel
        Log.i(TAG, "video panel: ${if (g.radius > 0f) "curved" else "flat"}, grab surface on root" +
            if (showGrabSurface) " (tinted)" else "")
    }

    // Root entity: pose of the screen, Grabbable, and a transparent box in its local space (+z points away from the
    // viewer). The root is the middle of the screen surface (the arc's middle when curved; the cylinder centre is
    // one radius back towards the viewer), so grabbing moves the visible screen rather than a point at the head.
    // Flat: the screen's size, 2 cm behind it. Curved: 2 cm behind the arc's middle and wide and tall enough that
    // every ray from the viewer to the arc reaches it (the arc is in front of the box everywhere).
    private fun createGrabRoot(pose: Pose, g: ScreenGeometry): Entity {
        val behind = 0.02f
        val (zCentre, halfW, halfH) = if (g.radius <= 0f) {
            Triple(behind, g.width / 2f, g.height / 2f)
        } else {
            val depthFromViewer = screenDistance + behind
            val edge = screenEdgeAngle(g).coerceAtMost(Math.toRadians(80.0).toFloat())
            val cosEdge = kotlin.math.cos(edge).coerceAtLeast(0.2f)
            Triple(
                behind,
                depthFromViewer * kotlin.math.tan(edge),
                (g.height / 2f) * depthFromViewer / (screenDistance * cosEdge),
            )
        }
        grabHalfW = halfW
        grabHalfH = halfH
        grabPlaneZ = zCentre
        val clear = Material().apply {
            baseColor = if (showGrabSurface) Color4(1f, 0f, 0.6f, 0.25f) else Color4(0f, 0f, 0f, 0f)
            unlit = true
            alphaMode = com.meta.spatial.runtime.AlphaMode.TRANSLUCENT.ordinal
        }
        return Entity.create(
            Transform(pose),
            Mesh(Uri.parse("mesh://box"), com.meta.spatial.toolkit.MeshCollision.LineTest),
            Box(Vector3(-halfW, -halfH, zCentre - 0.005f), Vector3(halfW, halfH, zCentre + 0.005f)),
            clear,
            Grabbable(true, screenGrabType),
        )
    }

    // Replaces the video panel with one built for the current settings. The new panel is created first; the old one
    // is destroyed when the new surface has been handed to the decoder (AMediaCodec_setOutputSurface, no decoder
    // restart). With an anchor (screen-setting changes) the new geometry is laid out around the same viewing point,
    // so the screen keeps its place instead of jumping to where the user looks; otherwise it keeps its pose.
    private fun rebuildVideoPanel(reason: String, anchor: Pose? = null) {
        val old = videoPanel ?: return
        val pose = videoRoot?.let { currentPose(it) } ?: Pose(Vector3(0.0f, 1.6f, screenDistance))
        retiringVideo = listOf(old) + videoFrame
        createVideoPanel(occlusionEnabled, pose)
        if (anchor != null) {
            screenAnchor = anchor
            placeFromAnchor(anchor)
        }
        Log.i(TAG, "video panel rebuilt ($reason): ${if (occlusionEnabled) "mesh" else "layer"}, ${describeScreen()}, " +
            "aspect %.3f".format(panelAspect))
    }

    // Screen settings from the control panel or launch extras; any null argument keeps its current value.
    fun setScreen(width: Float? = null, curve: Float? = null, fit: Float? = null, distance: Float? = null, fov: Float? = null) {
        runOnUiThread {
            // Viewing point for the current (old) geometry, taken before the settings change.
            val anchor = anchorFromVideoPanel()
            width?.let { screenWidth = it.coerceIn(0.4f, 12f) }
            curve?.let { screenCurve = it.coerceIn(0f, 1f) }
            fit?.let {
                fitDeg = if (it <= 0f) 0f else it.coerceIn(30f, 220f)
                resetScreenScale()    // a preset means its exact size
            }
            distance?.let { screenDistance = it.coerceIn(0.8f, 10f) }
            fov?.let { lensFovDeg = it.coerceIn(60f, 220f) }
            getSharedPreferences("questvisor", MODE_PRIVATE).edit()
                .putFloat("screenWidth", screenWidth).putFloat("screenCurve", screenCurve)
                .putFloat("fitDeg", fitDeg).remove("lensMatched").putFloat("screenDistance", screenDistance)
                .putFloat("fov", lensFovDeg).apply()
            if (videoPanel != null && retiringVideo.isEmpty()) {
                if (anchor != null) {
                    rebuildVideoPanel("screen settings", anchor)
                } else {
                    rebuildVideoPanel("screen settings")
                    recenterPanelsNowAndSoon()       // no layout yet: place in front of the user once
                }
                applyEnvironmentDepth()   // soft edges only apply to a flat screen
            }
        }
    }

    fun setOcclusion(enabled: Boolean) {
        runOnUiThread {
            if (enabled == occlusionEnabled) {
                return@runOnUiThread
            }
            occlusionEnabled = enabled
            getSharedPreferences("questvisor", MODE_PRIVATE).edit().putBoolean("occlusion", enabled).apply()
            rebuildVideoPanel("occlusion ${if (enabled) "on" else "off"}")
            applyEnvironmentDepth()
        }
    }

    // Soft edges apply to the flat occlusion panel; on a curved screen the panel keeps the SDK's hard occlusion.
    private fun softEdgesActive(): Boolean = occlusionEnabled && softEdges && screenGeometry().radius == 0f

    fun setSoftEdges(enabled: Boolean, reverseZ: Boolean? = null) {
        runOnUiThread {
            val changed = enabled != softEdges || (reverseZ != null && reverseZ != depthReverseZ)
            softEdges = enabled
            reverseZ?.let { depthReverseZ = it }
            getSharedPreferences("questvisor", MODE_PRIVATE).edit()
                .putBoolean("softEdges", softEdges).putBoolean("depthReverseZ", depthReverseZ).apply()
            if (changed && occlusionEnabled && videoPanel != null && retiringVideo.isEmpty()) {
                rebuildVideoPanel("soft edges ${if (softEdges) "on" else "off"}")
                applyEnvironmentDepth()
            }
        }
    }

    // Called when an occlusion (readable) panel has been created: in soft-edge mode its mesh is replaced by a quad of
    // the same size drawn with the soft-occlusion shader, sampling the panel's video texture.
    private fun onOccludedPanelCreated(panel: com.meta.spatial.runtime.PanelSceneObject) {
        if (!softEdgesActive()) {
            if (occlusionEnabled && softEdges) {
                Log.i(TAG, "soft edges: not available on a curved screen yet, using hard occlusion")
            }
            return
        }
        val videoTexture = panel.texture
        if (videoTexture == null) {
            Log.w(TAG, "soft edges: panel has no video texture yet, keeping hard occlusion")
            return
        }
        try {
            val shader = if (depthReverseZ) "softOcclusion" else "softOcclusionStd"
            val material = com.meta.spatial.runtime.SceneMaterial.custom(
                shader,
                arrayOf(com.meta.spatial.runtime.SceneMaterialAttribute(
                    "albedoSampler", com.meta.spatial.runtime.SceneMaterialDataType.Texture2D)),
                com.meta.spatial.runtime.AlphaMode.PREMULTIPLIED,
                com.meta.spatial.runtime.VertexLayout.COMPACT,
            )
            material.setTexture("albedoSampler", videoTexture)
            if (softUsesOcclusionPass) {
                // The SDK's occlusion pass puts the room depth in the depth buffer; skip the depth test so it does
                // not cut the video hard, and let the shader fade it against the same depth instead.
                material.setDepthTest(com.meta.spatial.runtime.DepthTest.ALWAYS)
                material.setDepthWrite(com.meta.spatial.runtime.DepthWrite.DISABLE)
            }
            val g = screenGeometry()
            val mesh = com.meta.spatial.runtime.SceneMesh.quad(
                Vector3(-g.width / 2f, -g.height / 2f, 0f), Vector3(g.width / 2f, g.height / 2f, 0f), material)
            panel.setSceneMesh(mesh, "softOcclusionVideo")
            Log.i(TAG, "soft edges: panel drawn with $shader (%.2f x %.2f m), depth source %s".format(
                g.width, g.height, if (softUsesOcclusionPass) "occlusion pass" else "texture only"))
        } catch (t: Throwable) {
            Log.w(TAG, "soft edges: custom material failed, keeping hard occlusion: $t")
        }
    }

    // Rebuilds the panel when the stream's aspect ratio changes (e.g. 16:9 -> race mode 4:3), so the picture is
    // never stretched. Sizes come from the decoder (stats[18], stats[19]); H.264 reports macroblock-aligned sizes
    // (1840x1040, 1920x1088), which are within 1 % of the real aspect.
    private fun checkStreamAspect() {
        val s = try { NativePlayer.stats() } catch (_: Throwable) { return }
        if (s.size < 20 || s[18] <= 0 || s[19] <= 0) {
            return
        }
        val aspect = s[18].toFloat() / s[19].toFloat()
        if (kotlin.math.abs(aspect - panelAspect) / panelAspect > 0.02f && videoPanel != null && retiringVideo.isEmpty()) {
            Log.i(TAG, "stream aspect %.3f (%dx%d), panel was %.3f".format(aspect, s[18], s[19], panelAspect))
            panelAspect = aspect
            rebuildVideoPanel("aspect change")
        }
    }

    private fun currentPose(entity: Entity): Pose? =
        try {
            entity.getComponent<Transform>().transform
        } catch (t: Throwable) {
            Log.w(TAG, "occlusion: could not read panel pose: $t")
            null
        }

    // Environment depth only runs while occlusion is on (it costs GPU and needs the spatial-data permission).
    private fun applyEnvironmentDepth() {
        if (occlusionEnabled && checkSelfPermission(USE_SCENE) != PackageManager.PERMISSION_GRANTED) {
            Log.i(TAG, "occlusion: requesting $USE_SCENE")
            requestPermissions(arrayOf(USE_SCENE), REQ_USE_SCENE)
            return
        }
        // Soft edges: the SDK only provides the depth texture (TEXTURE_ONLY); the own shader does the occlusion.
        val mode = when {
            !occlusionEnabled -> EnvironmentDepthMode.OFF
            softEdgesActive() && !softUsesOcclusionPass -> EnvironmentDepthMode.TEXTURE_ONLY
            else -> EnvironmentDepthMode.OCCLUSION
        }
        try {
            scene.setEnvironmentDepthMode(mode)
            Log.i(TAG, "environment depth: $mode")
        } catch (t: Throwable) {
            Log.w(TAG, "environment depth: setEnvironmentDepthMode($mode) failed: $t")
        }
    }

    override fun onRequestPermissionsResult(requestCode: Int, permissions: Array<out String>, grantResults: IntArray) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (requestCode != REQ_USE_SCENE) {
            return
        }
        val granted = grantResults.isNotEmpty() && grantResults[0] == PackageManager.PERMISSION_GRANTED
        Log.i(TAG, "occlusion: $USE_SCENE ${if (granted) "granted" else "denied"}")
        if (granted) {
            applyEnvironmentDepth()
        }
    }

    private fun onVideoSurface(kind: String, surface: android.view.Surface) {
        Log.i(TAG, "video surface received ($kind)")
        NativePlayer.setSurface(surface)
        surfaceReceived = true
        if (retiringVideo.isNotEmpty()) {
            val old = retiringVideo
            retiringVideo = emptyList()
            old.forEach { it.destroy() }
        }
        tryStartPending()
    }

    override fun registerPanels(): List<PanelRegistration> {
        return panelRegistrationList
    }

    override fun onRecenter(isUserInitiated: Boolean) {
        super.onRecenter(isUserInitiated)
        Log.i(TAG, "onRecenter(userInitiated=$isUserInitiated)")
        recenterPanelsNowAndSoon()
    }

    override fun onDestroy() {
        current = null
        autoConnectHandler.removeCallbacks(autoConnectTick)
        RelayDiscovery.stop()
        NativePlayer.stop()
        usbHelper.stop()
        super.onDestroy()
    }

    fun connectGoggles(
        h265: String = h265Decoder,
        h264: String = h264Decoder,
        decoderMode: String = pendingDecoderMode,
        hwexp: String = pendingHwExp,
    ) {
        if (!surfaceReceived) {
            pendingConnectRequested = true
            pendingSource = "usb"
            pendingDecoderMode = normalizeDecoderMode(decoderMode)
            pendingHwExp = hwexp
            return
        }

        val mode = normalizeDecoderMode(decoderMode)
        val hw = hwexp

        val host = getString(R.string.goggles_host)
        val port = getString(R.string.goggles_port).toIntOrNull() ?: 9100

        Thread {
            val networkFirst = UsbGoggles.tryNetwork(host, port, 1500)
            runOnUiThread {
                if (networkFirst) {
                    NativePlayer.startNetwork(host, port, h265, h264, mode, hw)
                    usbHelper.status = "USB: using native network interface"
                } else {
                    usbHelper.requestConnection(this, h265, h264, mode, hw)
                }
            }
        }.start()
    }

    fun stopUsb() {
        usbHelper.stop()
    }

    private fun loadFakeIdrAssets() {
        if (fakeAssetsLoaded) {
            return
        }
        fakeAssetsLoaded = true

        val dir = "fakeidr"
        val files = try {
            assets.list(dir) ?: emptyArray()
        } catch (_: Exception) {
            emptyArray<String>()
        }

        for (file in files) {
            if (file.endsWith(".json")) {
                continue
            }

            val codec = when {
                file.endsWith(".h265") -> "h265"
                file.endsWith(".h264") -> "h264"
                else -> continue
            }

            val m = fakeIdrRegex.find(file) ?: continue
            val width = m.groupValues[1].toIntOrNull() ?: continue
            val height = m.groupValues[2].toIntOrNull() ?: continue

            val bytes = try {
                assets.open("$dir/$file").use { it.readBytes() }
            } catch (_: Exception) {
                continue
            }

            if (bytes.isEmpty()) {
                continue
            }

            val sidecarJson = try {
                assets.open("$dir/$file.json").use { input ->
                    String(input.readBytes(), Charsets.UTF_8).trim()
                }
            } catch (_: Exception) {
                null
            }

            if (!sidecarJson.isNullOrEmpty()) {
                NativePlayer.addFakeIdrWithSidecar(codec, width, height, bytes, sidecarJson)
            } else {
                NativePlayer.addFakeIdr(codec, width, height, bytes)
            }
        }
    }

    private fun handleIntent(intent: Intent) {
        val prefs = getSharedPreferences("questvisor", MODE_PRIVATE)
        val editor = prefs.edit()
        var changed = false

        intent.getStringExtra("host")?.let {
            pendingHost = it
            editor.putString("host", it)
            changed = true
        }

        val port = intent.getIntExtra("port", -1)
        if (port > 0 && port <= 65535) {
            pendingPort = port
            editor.putString("port", port.toString())
            changed = true
        }

        intent.getStringExtra("source")?.let {
            pendingSource = if (it.equals("usb", ignoreCase = true)) "usb" else "network"
            editor.putString("source", pendingSource)
            changed = true
        }

        val decoder = intent.getStringExtra("decoder")
        if (!decoder.isNullOrBlank()) {
            pendingDecoderMode = normalizeDecoderMode(decoder)
            editor.putString("decoder", pendingDecoderMode)
            changed = true
        }

        val hwexp = intent.getStringExtra("hwexp")
        if (hwexp != null) {
            pendingHwExp = hwexp
            editor.putString("hwexp", hwexp)
            changed = true
        }

        val auto = intent.getBooleanExtra("autoconnect", false)
        if (auto) {
            pendingConnectRequested = true
        }

        if (intent.hasExtra("occlusion")) {
            val occ = intent.getBooleanExtra("occlusion", false)
            if (videoPanel == null) {
                occlusionEnabled = occ              // scene not up yet: onSceneReady creates the right panel
                editor.putBoolean("occlusion", occ)
                changed = true
            } else {
                setOcclusion(occ)
            }
        }

        // Soft edges: --ez softedges <bool>, --es depthz rev|std (shader depth convention).
        intent.getStringExtra("softdepth")?.let { softUsesOcclusionPass = it != "texture" }
        if (intent.hasExtra("showgrab")) {
            showGrabSurface = intent.getBooleanExtra("showgrab", false)   // diagnostic; applies at the next rebuild
        }
        if (intent.hasExtra("softedges") || intent.hasExtra("depthz")) {
            val soft = intent.getBooleanExtra("softedges", softEdges)
            val z = intent.getStringExtra("depthz")?.let { it != "std" }
            setSoftEdges(soft, z)
        }

        if (intent.hasExtra("headlock")) {
            val lock = intent.getBooleanExtra("headlock", headLocked)
            if (videoPanel == null) {
                headLocked = lock
                editor.putBoolean("headLocked", lock)
                changed = true
            } else {
                setHeadLock(lock)
            }
        }

        // Screen geometry extras: --ef screenwidth <m>, --ef curve <0..1>, --ez lens155 <bool>, --ef distance <m>,
        // --ef fov <deg diagonal>.
        val sw = if (intent.hasExtra("screenwidth")) intent.getFloatExtra("screenwidth", screenWidth) else null
        val sc = if (intent.hasExtra("curve")) intent.getFloatExtra("curve", screenCurve) else null
        // --ef fit <deg> (0 off, 100, 155); --ez lens155 true|false is the older form of fit 155.
        val sl = when {
            intent.hasExtra("fit") -> intent.getFloatExtra("fit", fitDeg)
            intent.hasExtra("lens155") -> if (intent.getBooleanExtra("lens155", false)) 155f else 0f
            else -> null
        }
        val sd = if (intent.hasExtra("distance")) intent.getFloatExtra("distance", screenDistance) else null
        val sf = if (intent.hasExtra("fov")) intent.getFloatExtra("fov", lensFovDeg) else null
        if (sw != null || sc != null || sl != null || sd != null || sf != null) {
            setScreen(sw, sc, sl, sd, sf)
        }

        // Replay test hooks: --ei replaybuffer <s>, --ei replayback <s>, --ez replaypause <bool>, --ez live true.
        if (intent.hasExtra("replaybuffer")) applyReplayBuffer(intent.getIntExtra("replaybuffer", replaySeconds))
        if (intent.hasExtra("replayback")) replayBack(intent.getIntExtra("replayback", 30))
        if (intent.hasExtra("replaypause")) setReplayPaused(intent.getBooleanExtra("replaypause", false))
        if (intent.getBooleanExtra("live", false)) goLive()

        val stats = intent.getBooleanExtra("stats", false)
        editor.putBoolean("showStats", stats)
        changed = true

        val selftest = intent.getStringExtra("selftest")
        if (!selftest.isNullOrBlank()) {
            pendingSelftestPath = resolveSelftestPath(selftest)
        }

        if (intent.action == "android.hardware.usb.action.USB_DEVICE_ATTACHED") {
            pendingConnectRequested = true
            pendingSource = "usb"
        }

        if (changed) {
            editor.apply()
        }

        tryStartPending()
    }

    private fun resolveSelftestPath(path: String): String {
        if (path.startsWith("/")) {
            return path
        }
        val root = getExternalFilesDir(null)?.absolutePath ?: filesDir.absolutePath
        return "$root/$path"
    }

    private fun tryStartPending() {
        if (!surfaceReceived) {
            return;
        }

        if (!pendingSelftestPath.isNullOrBlank()) {
            NativePlayer.startSelftest(
                pendingSelftestPath!!,
                h265Decoder,
                h264Decoder,
                pendingDecoderMode,
                pendingHwExp,
            )
            pendingSelftestPath = null
            pendingConnectRequested = false
            return
        }

        if (pendingConnectRequested) {
            pendingConnectRequested = false

            if (pendingSource == "usb") {
                connectGoggles(
                    h265Decoder,
                    h264Decoder,
                    pendingDecoderMode,
                    pendingHwExp,
                )
            } else {
                val host = pendingHost.ifBlank {
                    getSharedPreferences("questvisor", MODE_PRIVATE)
                        .getString("host", getString(R.string.default_host))
                        ?: getString(R.string.default_host)
                }
                val port = if (pendingPort > 0) {
                    pendingPort
                } else {
                    (getSharedPreferences("questvisor", MODE_PRIVATE)
                        .getString("port", getString(R.string.default_port))
                        ?: getString(R.string.default_port))
                        .toIntOrNull() ?: 9201
                }

                startNetwork(host, port)
            }
        }
    }

    /** Connects to the relay at host:port over the network and remembers it as the last relay. */
    fun startNetwork(host: String, port: Int) {
        currentHost = host
        currentPort = port
        userStopped = false
        connectingSinceMs = android.os.SystemClock.elapsedRealtime()
        getSharedPreferences("questvisor", MODE_PRIVATE).edit()
            .putString("host", host).putString("port", port.toString()).apply()
        Log.i(TAG, "connect: $host:$port")
        pendingHwExp = getSharedPreferences("questvisor", MODE_PRIVATE).getString("hwexp", pendingHwExp) ?: pendingHwExp
        NativePlayer.startNetwork(host, port, h265Decoder, h264Decoder, pendingDecoderMode, pendingHwExp)
    }

    // Replay buffer (see replay_recorder.h). Stored in the app's own folder; removed with the app.
    fun applyReplayBuffer(seconds: Int) {
        replaySeconds = seconds
        getSharedPreferences("questvisor", MODE_PRIVATE).edit().putInt("replaySeconds", seconds).apply()
        val dir = java.io.File(getExternalFilesDir(null) ?: filesDir, "replay")
        NativePlayer.setReplayBuffer(dir.absolutePath, seconds)
    }

    /** Starts (or moves) playback [seconds] back from the replay position, or from live when not replaying. */
    fun replayBack(seconds: Int) {
        val info = NativePlayer.replayInfo()
        if (info[3] == 0L) return                       // nothing recorded
        val from = if (replaying && info[0] != 0L && info[1] > 0L) info[1] else info[3]
        val startMs = maxOf(info[2], from - seconds * 1000L)
        replaying = true
        Log.i(TAG, "replay: from ${(info[3] - startMs) / 1000} s before the end of the buffer")
        NativePlayer.startReplay(startMs, h265Decoder, h264Decoder, pendingDecoderMode, pendingHwExp)
    }

    fun setReplayPaused(paused: Boolean) = NativePlayer.setReplayPaused(paused)

    /** Back to the live feed (recording resumes). */
    fun goLive() {
        replaying = false
        NativePlayer.stop()
        currentHost = ""
        userStopped = false
        val source = getSharedPreferences("questvisor", MODE_PRIVATE).getString("source", "network") ?: "network"
        if (source == "usb") connectGoggles() else autoConnect()
    }

    fun stopNetwork() {
        userStopped = true
        NativePlayer.stop()
        currentHost = ""
    }

    // Once a second: with nothing playing, connect to the relay that discovery heard (or, if none has been heard
    // within 3 s of start, to the last relay used). While stuck connecting to an address no relay announces from
    // (the PC got a new address), switch to the announced one.
    private fun autoConnect() {
        if (!surfaceReceived || userStopped || replaying || pendingSelftestPath != null) return
        val prefs = getSharedPreferences("questvisor", MODE_PRIVATE)
        if ((prefs.getString("source", "network") ?: "network") != "network") return
        val relay = RelayDiscovery.current()
        val state = NativePlayer.uiState()
        val now = android.os.SystemClock.elapsedRealtime()
        if (state == 0) {
            if (relay != null) {
                startNetwork(relay.host, relay.port)
            } else if (now - startedAtMs > 3000) {
                val host = prefs.getString("host", getString(R.string.default_host)) ?: ""
                val port = (prefs.getString("port", getString(R.string.default_port)) ?: "").toIntOrNull() ?: 9201
                if (host.isNotBlank()) startNetwork(host, port)
            }
        } else if (state == 1 && relay != null && (relay.host != currentHost || relay.port != currentPort) &&
            now - connectingSinceMs > 4000
        ) {
            Log.i(TAG, "connect: no stream from $currentHost:$currentPort, relay announced at ${relay.host}")
            NativePlayer.stop()
            startNetwork(relay.host, relay.port)
        }
    }

    // Decoding is always hardware (c2.qti.*.low_latency). The software decoder and "auto" (hardware with software
    // fallback) are no longer selectable: any saved setting or `decoder` launch extra resolves to "hw".
    @Suppress("UNUSED_PARAMETER")
    private fun normalizeDecoderMode(mode: String?): String = "hw"

    // Places both panels in front of the viewer's current yaw. The viewer pose comes from Scene (the earlier
    // reflection looked for it on the activity / VRFeature, never found it, and silently left the panels at their
    // fixed start coordinates, which a system recenter can put behind the user). Forward and the yaw-only
    // rotation come from the SDK's own Pose helpers, so no axis convention is assumed here.
    private fun recenterPanels() {
        val head = try {
            scene.getViewerPose()
        } catch (t: Throwable) {
            Log.w(TAG, "recenter: getViewerPose failed: $t")
            return
        }
        val level = head.removePitchAndRoll()
        val flat = Vector3(level.forward().x, 0f, level.forward().z)
        if (flat.length() < 0.001f) {
            Log.w(TAG, "recenter: no usable forward vector, pose $head")
            return
        }
        // Viewer height in the scene's floor-level space; fall back to a standing height before tracking settles.
        val eyeY = if (head.t.y > 0.5f) head.t.y else 1.6f
        screenAnchor = Pose(Vector3(head.t.x, eyeY, head.t.z), level.q)
        placeFromAnchor(screenAnchor!!)
        resetTilt()                                    // a recenter brings the screen back upright, facing you
        Log.i(TAG, "recenter: head $head -> panels along ${flat.normalize()}")
    }

    // Viewing point (eye position) and yaw the screen was last laid out for. Set by a recenter; screen-setting
    // changes keep it, so changing size or curve reshapes the screen in place instead of moving it.
    private var screenAnchor: Pose? = null

    // Places the video screen and the control panel for a viewing point. Video: centre (flat) or arc middle
    // (curved) at the configured distance straight ahead of the anchor. Control panel (with the stats column):
    // just past the screen's right edge, 1.2 m from the anchor, turned to face it.
    private fun placeFromAnchor(anchor: Pose) {
        val fwd = anchor.forward().let { Vector3(it.x, 0f, it.z) }
        if (fwd.length() < 0.001f) return
        val forward = fwd.normalize()
        val g = screenGeometry()
        val along = screenDistance   // root = middle of the screen surface (arc middle when curved)
        if (!headLocked) {
            videoRoot?.setComponent(Transform(Pose(anchor.t + forward * along, anchor.q)))
        }
        val right = anchor.right().let { Vector3(it.x, 0f, it.z).normalize() }
        val edge = screenEdgeAngle(g)
        val controlHalf = kotlin.math.atan2(controlWidth / 2f, controlDistance)
        val angle = (edge + Math.toRadians(4.0).toFloat() + controlHalf).coerceAtMost(Math.toRadians(115.0).toFloat())
        val dir = (forward * kotlin.math.cos(angle) + right * kotlin.math.sin(angle)).normalize()
        val facing = Quaternion.fromTwoVectors(forward, dir) * anchor.q
        // The settings panel and its button are children of the screen root (attachMenuButton), so they move with
        // the screen; nothing to place here. (angle/dir/facing above are kept for the log only.)
        Log.i(TAG, "layout: screen edge %.0f deg; settings under the screen".format(Math.toDegrees(edge.toDouble())))
    }

    // Anchor implied by where the video screen is now (it may have been grabbed and moved), for the geometry it
    // was built with: the viewing point is `along` metres behind the entity along its facing direction.
    private fun anchorFromVideoPanel(): Pose? {
        if (headLocked) return screenAnchor
        val pose = videoRoot?.let { currentPose(it) } ?: return screenAnchor
        val g = screenGeometry()
        val along = screenDistance   // root = middle of the screen surface (arc middle when curved)
        val fwd = pose.forward().let { Vector3(it.x, 0f, it.z) }
        if (fwd.length() < 0.001f) return screenAnchor
        val anchorY = screenAnchor?.t?.y ?: pose.t.y
        val t = pose.t + fwd.normalize() * (-along)
        return Pose(Vector3(t.x, anchorY, t.z), pose.q)
    }

    private val controlWidth = 1.1f
    private val controlDistance = 1.2f

    // Places the video screen relative to the head. levelOnly (recenter): yaw only, at eye height, so the screen
    // stands upright in the room. Otherwise (head lock, every frame): the full head pose, so the screen stays
    // straight in front of the eyes when looking up, down or tilting the head.
    // Flat: screen centre at the configured distance. Curved: the entity is the cylinder's centre (as for an OpenXR
    // cylinder layer), placed so the arc's middle is at the configured distance; for a curve of 100 % (radius =
    // distance, and always in lens-matched mode) that is the head itself.
    private fun placeVideoAtHead(levelOnly: Boolean, head: Pose? = null) {
        val panel = videoRoot ?: return
        val h = head ?: try { scene.getViewerPose() } catch (_: Throwable) { return }
        val g = screenGeometry()
        val along = screenDistance   // root = middle of the screen surface (arc middle when curved)
        if (levelOnly) {
            val level = h.removePitchAndRoll()
            val flat = Vector3(level.forward().x, 0f, level.forward().z)
            if (flat.length() < 0.001f) return
            val eyeY = if (h.t.y > 0.5f) h.t.y else 1.6f
            panel.setComponent(Transform(Pose(Vector3(h.t.x, eyeY, h.t.z) + flat.normalize() * along, level.q)))
        } else {
            if (h.t.length() < 0.0001f) return            // no tracking yet
            panel.setComponent(Transform(Pose(h.t + h.forward() * along, h.q)))
        }
    }

    // Settings button: a child of the screen root, so it follows grabbing, head lock and scaling. Centred under the
    // screen's bottom edge (root = middle of the screen surface) and 25 cm in front of it, facing the viewer.
    // The settings panel's size (control panel registration: 1.1 x 0.45 m).
    private val settingsPanelHeight = 0.45f

    // Settings button and settings panel are children of the screen root, so they follow grabbing, head lock and
    // scaling. Button centred under the screen's bottom edge, 25 cm in front of the screen surface; the settings
    // panel hangs directly below the button, in the same plane, facing the viewer.
    private fun attachMenuButton() {
        val root = videoRoot ?: return
        val g = screenGeometry()
        // In root-local units (scaled by the two-handed screen scale). The Settings button scales with the screen, so
        // it never jumps on release. The settings panel keeps one size: it is counter-scaled, and since it is hidden
        // whenever the scale changes (and stays hidden after), that never shows as a jump either.
        val inv = 1f / screenScale
        // Cog: just outside the screen, past its lower-right corner, level with the bottom edge and in the screen's
        // own surface (flat plane, or the same arc when curved). On a curved screen the cylinder centre is one radius
        // towards the viewer from the root, so a point at arc angle phi is at (r sin phi, y, -r + r cos phi), turned
        // to face the centre.
        val outside = 0.015f + menuButtonWidth / 2f
        val cogY = -g.height / 2f + menuButtonHeight / 2f
        val cogPose = if (g.radius > 0f) {
            val phi = (g.width / 2f + outside) / g.radius
            val radial = Vector3(kotlin.math.sin(phi), 0f, kotlin.math.cos(phi))
            Pose(Vector3(g.radius * radial.x, cogY, -g.radius + g.radius * radial.z), Quaternion.fromTwoVectors(Vector3(0f, 0f, 1f), radial))
        } else {
            Pose(Vector3(g.width / 2f + outside, cogY, 0f))
        }
        val parent = videoTilt ?: root
        menuButton?.let {
            it.setComponent(TransformParent(parent))
            it.setComponent(Transform(cogPose))
        }
        // Settings panel: under the screen's bottom edge, centred below the cog, a little in front of the screen.
        // Fixed size (counter-scaled; it is hidden whenever the scale changes, so that never shows as a jump).
        val panelY = -g.height / 2f - 0.04f - (settingsPanelHeight / 2f) * inv
        controlPanel?.let {
            it.setComponent(TransformParent(parent))
            it.setComponent(Transform(Pose(Vector3(cogPose.t.x, panelY, cogPose.t.z - 0.05f), cogPose.q)))
            it.setComponent(com.meta.spatial.toolkit.Scale(Vector3(inv, inv, inv)))
        }
    }

    // Tilt layer rotation: yaw about the root's vertical, then pitch about the resulting horizontal axis (built from
    // vectors, so no roll and no dependence on axis or multiplication conventions beyond fromTwoVectors).
    private fun applyTilt() {
        val tilt = videoTilt ?: return
        val flat = tiltYaw.times(Vector3(0f, 0f, 1f)).let { Vector3(it.x, 0f, it.z).normalize() }
        val pitched = flat * kotlin.math.cos(tiltPitch) + Vector3(0f, 1f, 0f) * kotlin.math.sin(tiltPitch)
        val rotation = (Quaternion.fromTwoVectors(flat, pitched) * tiltYaw).normalize()
        tilt.setComponent(Transform(Pose(Vector3(0f, 0f, 0f), rotation)))
    }

    private fun resetTilt() {
        tiltYaw = Quaternion(1f, 0f, 0f, 0f)
        tiltPitch = 0f
        applyTilt()
    }

    // Shows or hides the settings panel (it stays attached under the Settings button).
    fun setSettingsVisible(visible: Boolean) {
        runOnUiThread {
            settingsVisible = visible
            getSharedPreferences("questvisor", MODE_PRIVATE).edit().putBoolean("settingsVisible", visible).apply()
            showSettingsPanel(visible)
            Log.i(TAG, "settings panel ${if (visible) "shown under the Settings button" else "hidden"}")
        }
    }

    // Shows or hides the settings panel without changing the saved setting (also used to hide it while scaling).
    private fun showSettingsPanel(visible: Boolean) {
        controlPanel?.setComponent(com.meta.spatial.toolkit.Visible(visible))
        // The frame boxes are separate entities: hide them with the panel so no border is left floating.
        controlFrame.forEach { it.setComponent(com.meta.spatial.toolkit.Visible(visible)) }
    }

    fun setHeadLock(enabled: Boolean) {
        runOnUiThread {
            if (enabled == headLocked) return@runOnUiThread
            val anchor = anchorFromVideoPanel()
            headLocked = enabled
            if (enabled) resetTilt()                   // head lock follows the head exactly
            getSharedPreferences("questvisor", MODE_PRIVATE).edit().putBoolean("headLocked", enabled).apply()
            Log.i(TAG, "head lock ${if (enabled) "on: screen follows the headset" else "off: screen stays in the room"}")
            // The size may change (capped at 100 deg while locked), so rebuild around the current layout.
            if (videoPanel != null && retiringVideo.isEmpty()) {
                rebuildVideoPanel("head lock ${if (enabled) "on" else "off"}", anchor)
            }
            if (!enabled) {
                recenterPanelsNowAndSoon()             // back to an upright, room-fixed screen in front of the user
            }
        }
    }

    // Horizontal angle (radians) from straight ahead to the screen's right edge, as seen from the head.
    // Flat: atan(half width / distance). Curved: the edge sits at angle w/2r on a cylinder whose centre is
    // (distance - radius) in front of the head.
    private fun screenEdgeAngle(g: ScreenGeometry): Float {
        if (g.radius <= 0f) {
            return kotlin.math.atan2(g.width / 2f, screenDistance)
        }
        val phi = g.width / (2f * g.radius)
        return kotlin.math.atan2(g.radius * kotlin.math.sin(phi), (screenDistance - g.radius) + g.radius * kotlin.math.cos(phi))
    }

    // A recenter or scene start can report the pose of the frame before the change; place again shortly after.
    private fun recenterPanelsNowAndSoon() {
        recenterPanels()
        window.decorView.postDelayed({ recenterPanels() }, 300)
    }

    private fun tryInvokeComponentMethod(entity: Entity, methodName: String, component: Any): Boolean {
        var clazz: Class<*>? = entity.javaClass
        while (clazz != null) {
            for (method in clazz.declaredMethods) {
                if (method.name != methodName) {
                    continue
                }

                try {
                    method.setAccessible(true)
                } catch (_: Throwable) {
                }

                if (method.parameterCount == 1) {
                    try {
                        method.invoke(entity, component)
                        return true
                    } catch (_: Throwable) {
                    }

                    try {
                        method.invoke(entity, arrayOf(component))
                        return true
                    } catch (_: Throwable) {
                    }
                }
            }

            clazz = clazz.superclass
        }

        return false
    }

    private fun findNoArgMethod(target: Any?, name: String): Method? {
        if (target == null) {
            return null
        }

        var clazz: Class<*>? = target.javaClass
        while (clazz != null) {
            try {
                val method = clazz.getMethod(name)
                if (method.parameterCount == 0) {
                    return method
                }
            } catch (_: Throwable) {
            }

            try {
                val method = clazz.getDeclaredMethod(name)
                if (method.parameterCount == 0) {
                    try {
                        method.setAccessible(true)
                    } catch (_: Throwable) {
                    }
                    return method
                }
            } catch (_: Throwable) {
            }

            clazz = clazz.superclass
        }

        return null
    }

    private fun readMethodValue(target: Any?, name: String): Any? {
        if (target == null || name.isEmpty()) {
            return null
        }

        val cap = name.replaceFirstChar { c -> c.uppercaseChar() }
        for (methodName in listOf("get$cap", "is$cap", name)) {
            val method = findNoArgMethod(target, methodName) ?: continue
            try {
                val value = method.invoke(target)
                if (value != null) {
                    return value
                }
            } catch (_: Throwable) {
            }
        }

        return readFieldValue(target, name)
    }

    private fun readFieldValue(target: Any?, name: String): Any? {
        if (target == null) {
            return null
        }

        var clazz: Class<*>? = target.javaClass
        while (clazz != null) {
            try {
                val field = clazz.getDeclaredField(name)
                field.setAccessible(true)
                return field.get(target)
            } catch (_: Throwable) {
            }
            clazz = clazz.superclass
        }

        return null
    }

    private fun readFloat(target: Any?, name: String): Float? {
        val value = readMethodValue(target, name) ?: return null
        return if (value is Number) value.toFloat() else null
    }

    private fun readVector3(target: Any?, name: String): Vector3? {
        val vec = readMethodValue(target, name) ?: return null
        val x = readFloat(vec, "x") ?: return null
        val y = readFloat(vec, "y") ?: return null
        val z = readFloat(vec, "z") ?: return null
        return Vector3(x, y, z)
    }

    private fun readQuaternion(target: Any?, name: String): Quaternion? {
        val q = readMethodValue(target, name) ?: return null
        val x = readFloat(q, "x") ?: return null
        val y = readFloat(q, "y") ?: return null
        val z = readFloat(q, "z") ?: return null
        val w = readFloat(q, "w") ?: return null
        return Quaternion(x, y, z, w)
    }

    private fun createFrame(
        parent: Entity,
        width: Float,
        height: Float,
        frameThickness: Float,
        depth: Float,
    ): List<Entity> {
        val boxes = mutableListOf<Entity>()
        val halfW = width * 0.5f
        val halfH = height * 0.5f
        val t = frameThickness
        val d = depth

        val meshUri = Uri.parse("mesh://box")
        val material = Material().apply {
            baseColor = Color4(0.85f, 0.85f, 0.9f, 1f)
        }

        fun addBox(
            minX: Float,
            minY: Float,
            minZ: Float,
            maxX: Float,
            maxY: Float,
            maxZ: Float,
        ) {
            val pos = Vector3(
                (minX + maxX) * 0.5f,
                (minY + maxY) * 0.5f,
                (minZ + maxZ) * 0.5f,
            )

            val sizeX = (maxX - minX) * 0.5f
            val sizeY = (maxY - minY) * 0.5f
            val sizeZ = (maxZ - minZ) * 0.5f

            val localMin = Vector3(-sizeX, -sizeY, -sizeZ)
            val localMax = Vector3(sizeX, sizeY, sizeZ)

            boxes += Entity.create(
                Transform(Pose(pos)),
                Mesh(meshUri),
                Box(localMin, localMax),
                material,
                TransformParent(parent),
            )
        }

        addBox(-halfW - t, halfH, -d * 0.5f, halfW + t, halfH + t, d * 0.5f)
        addBox(-halfW - t, -halfH - t, -d * 0.5f, halfW + t, -halfH, d * 0.5f)
        addBox(-halfW - t, -halfH, -d * 0.5f, -halfW, halfH, d * 0.5f)
        addBox(halfW, -halfH, -d * 0.5f, halfW + t, halfH, d * 0.5f)
        return boxes
    }

    // Default: MediaCodec renders straight into a compositor layer (lowest latency, never occluded).
    private fun videoPanelRegistration(): VideoSurfacePanelRegistration {
        return VideoSurfacePanelRegistration(
            R.id.video_panel,
            surfaceConsumer = { _, surface -> onVideoSurface("layer", surface) },
            settingsCreator = {
                MediaPanelSettings(
                    shape = videoShape(),
                    display = PixelDisplayOptions(width = 1832, height = 1032),
                    // Below the UI panels: the settings panel and its button are always drawn over the video,
                    // even where a large or head-locked screen is in front of them.
                    rendering = MediaPanelRenderOptions(zIndex = -1),
                    style = PanelStyleOptions(R.style.ThemeTransparent),
                )
            },
        )
    }

    // Occlusion mode: the decoder renders into a readable surface that the app draws as a mesh in the scene, where
    // environment depth can cut real objects (hands) out of it. One mip level: no mip chain to rebuild per frame.
    private fun occludedVideoPanelRegistration(): ReadableVideoSurfacePanelRegistration {
        return ReadableVideoSurfacePanelRegistration(
            R.id.video_panel_occluded,
            { _, surface -> onVideoSurface("mesh", surface) },
            {
                ReadableMediaPanelSettings(
                    shape = videoShape(),
                    display = PixelDisplayOptions(width = 1832, height = 1032),
                    rendering = ReadableMediaPanelRenderOptions(mips = 1, renderMode = PanelRenderMode.Mesh()),
                    style = PanelStyleOptions(R.style.ThemeTransparent),
                )
            },
            { panel, _ -> onOccludedPanelCreated(panel) },
        )
    }

    // Small "Settings" button that sits under the video screen and shows / hides the settings panel.
    private fun menuButtonRegistration(): ComposeViewPanelRegistration {
        return ComposeViewPanelRegistration(
            R.id.menu_button,
            composeViewCreator = { _, ctx ->
                ComposeView(ctx).apply {
                    setContent {
                        SettingsToggleButton()
                    }
                }
            },
            settingsCreator = {
                UIPanelSettings(
                    shape = QuadShapeOptions(menuButtonWidth, menuButtonHeight),
                    style = PanelStyleOptions(R.style.ThemeTransparent),
                    display = DpDisplayOptions(width = 72f, height = 72f, dpi = 260),
                )
            },
        )
    }

    private fun controlPanelRegistration(): ComposeViewPanelRegistration {
        return ComposeViewPanelRegistration(
            R.id.control_panel,
            composeViewCreator = { _, ctx ->
                ComposeView(ctx).apply {
                    setContent {
                        ControlPanel(ctx)
                    }
                }
            },
            settingsCreator = {
                UIPanelSettings(
                    shape = QuadShapeOptions(1.1f, 0.45f),
                    style = PanelStyleOptions(R.style.ThemeTransparent),
                    display = DpDisplayOptions(width = 1000f, height = 420f, dpi = 260),
                )
            },
        )
    }
}
