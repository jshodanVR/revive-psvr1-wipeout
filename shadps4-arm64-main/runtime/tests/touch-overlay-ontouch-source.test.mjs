import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import test from "node:test";

const root = resolve(dirname(fileURLToPath(import.meta.url)), "../..");
const overlay = readFileSync(
  resolve(root, "android/BachataS4/feature/session/src/main/kotlin/com/bachatas4/android/feature/session/controller/FixedControllerOverlay.kt"),
  "utf8",
);

test("touch overlay handles MotionEvent on the Android View, not Compose pointerInput on AndroidView", () => {
  assert.match(
    overlay,
    /override fun onTouchEvent\(event: MotionEvent\): Boolean/,
    "GlassOverlayView must receive taps; Compose pointerInput on AndroidView is often never dispatched",
  );
  assert.match(overlay, /onMotion\?\.invoke\(event\)/);
  const androidView = overlay.slice(
    overlay.indexOf("AndroidView("),
    overlay.indexOf("private fun visualColor"),
  );
  assert.doesNotMatch(
    androidView,
    /pointerInput\(/,
    "pointerInput on AndroidView modifier loses MotionEvents to the View interop",
  );
});
