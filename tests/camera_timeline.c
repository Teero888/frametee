#include <assert.h>
#include <math.h>
#include <string.h>
#include <user_interface/camera/camera_timeline.h>

static const double TPS = 50.0;

static void near(double actual, double expected) { assert(fabs(actual - expected) < 1e-3); }

static camera_pose_t at_x(float x) {
  camera_pose_t pose = {.zoom = 1.f, .fov_y = 1.f};
  pose.eye[0] = x;
  pose.target[0] = x;
  pose.target[2] = 1.f;
  return pose;
}

static void game_time(void) {
  camera_timeline_t camera;
  camera_timeline_default(&camera);
  near(camera_game_tick(&camera, 2.0, TPS), 100.0); // no keys: 1x

  // Freeze between 1s and 2s, 1x on either side.
  assert(camera_insert_time_key(&camera, 1.0, 50.0, CAMERA_INTERP_LINEAR) == 0);
  assert(camera_insert_time_key(&camera, 2.0, 50.0, CAMERA_INTERP_LINEAR) == 1);
  near(camera_game_tick(&camera, 0.5, TPS), 25.0);
  near(camera_game_tick(&camera, 1.5, TPS), 50.0);
  near(camera_game_tick(&camera, 3.0, TPS), 100.0);
  double seconds;
  assert(camera_seconds_for_game_tick(&camera, 50.0, TPS, 0.0, &seconds));
  near(seconds, 1.0);
  assert(camera_seconds_for_game_tick(&camera, 75.0, TPS, 0.0, &seconds));
  near(seconds, 2.5);

  // Smooth keys keep a freeze exactly flat and never run backwards.
  for (int i = 0; i < camera.time_count; ++i) camera.time_keys[i].interp = CAMERA_INTERP_SMOOTH;
  assert(camera_insert_time_key(&camera, 0.0, 0.0, CAMERA_INTERP_SMOOTH) == 0);
  assert(camera_insert_time_key(&camera, 3.0, 100.0, CAMERA_INTERP_SMOOTH) == 3);
  near(camera_game_tick(&camera, 1.25, TPS), 50.0);
  near(camera_game_tick(&camera, 1.75, TPS), 50.0);
  double previous = camera_game_tick(&camera, 0.0, TPS);
  for (double t = 0.01; t <= 3.0; t += 0.01) {
    const double value = camera_game_tick(&camera, t, TPS);
    assert(value >= previous - 1e-9);
    previous = value;
  }

  // Reverse: forward to tick 50, back to 0, then 1x again.
  camera_timeline_default(&camera);
  camera_insert_time_key(&camera, 0.0, 0.0, CAMERA_INTERP_LINEAR);
  camera_insert_time_key(&camera, 1.0, 50.0, CAMERA_INTERP_LINEAR);
  camera_insert_time_key(&camera, 2.0, 0.0, CAMERA_INTERP_LINEAR);
  near(camera_game_tick(&camera, 1.5, TPS), 25.0);
  near(camera_game_tick(&camera, 2.5, TPS), 25.0);
  assert(camera_seconds_for_game_tick(&camera, 25.0, TPS, 0.0, &seconds));
  near(seconds, 0.5);
  assert(camera_seconds_for_game_tick(&camera, 25.0, TPS, 0.6, &seconds));
  near(seconds, 1.5);
  assert(camera_seconds_for_game_tick(&camera, 25.0, TPS, 1.6, &seconds));
  near(seconds, 2.5);

  // A held last key freezes the game for good.
  camera_timeline_default(&camera);
  camera_insert_time_key(&camera, 1.0, 50.0, CAMERA_INTERP_HOLD);
  near(camera_game_tick(&camera, 5.0, TPS), 50.0);
  assert(!camera_seconds_for_game_tick(&camera, 60.0, TPS, 0.0, &seconds));

  // Hold between keys jumps the game at the next key.
  camera_timeline_default(&camera);
  camera_insert_time_key(&camera, 0.0, 0.0, CAMERA_INTERP_HOLD);
  camera_insert_time_key(&camera, 1.0, 200.0, CAMERA_INTERP_LINEAR);
  near(camera_game_tick(&camera, 0.99, TPS), 0.0);
  near(camera_game_tick(&camera, 1.0, TPS), 200.0);
}

static void pose(void) {
  camera_timeline_t camera;
  camera_timeline_default(&camera);
  camera_pose_t a = at_x(0.f), b = at_x(1.f), c = at_x(2.f);
  camera_insert_pose_key(&camera, 0.0, &a, CAMERA_INTERP_SMOOTH);
  camera_insert_pose_key(&camera, 1.0, &b, CAMERA_INTERP_SMOOTH);
  camera_insert_pose_key(&camera, 2.0, &c, CAMERA_INTERP_SMOOTH);
  near(camera_eval_pose(&camera, 0.0).eye[0], 0.0);
  near(camera_eval_pose(&camera, 2.0).eye[0], 2.0);
  // Passes through the middle key without stopping, starts and ends at rest.
  const double through = (camera_eval_pose(&camera, 1.01).eye[0] - camera_eval_pose(&camera, 0.99).eye[0]) / 0.02;
  assert(through > 0.9);
  const double start = camera_eval_pose(&camera, 0.001).eye[0] / 0.001;
  assert(start < 0.05);

  // A key on the same spot as its neighbour holds still between them.
  camera_pose_t d = at_x(1.f);
  camera.pose_keys[2].time = 3.0;
  camera.pose_keys[2].pose.eye[0] = 2.f;
  camera_insert_pose_key(&camera, 2.0, &d, CAMERA_INTERP_SMOOTH);
  near(camera_eval_pose(&camera, 1.5).eye[0], 1.0);

  // Hold is a cut.
  camera.pose_keys[0].interp = CAMERA_INTERP_HOLD;
  near(camera_eval_pose(&camera, 0.9).eye[0], 0.0);

  // Zoom blends as a scale.
  camera_timeline_default(&camera);
  camera_pose_t wide = at_x(0.f), close = at_x(0.f);
  close.zoom = 4.f;
  camera_insert_pose_key(&camera, 0.0, &wide, CAMERA_INTERP_LINEAR);
  camera_insert_pose_key(&camera, 1.0, &close, CAMERA_INTERP_LINEAR);
  near(camera_eval_pose(&camera, 0.5).zoom, 2.0);
}

static void influence(void) {
  camera_timeline_t camera;
  camera_timeline_default(&camera);
  near(camera_eval_follow(&camera, 1.0), 0.0);
  camera_insert_influence_key(&camera, CAMERA_CHANNEL_FOLLOW, 0.0, 0.f, CAMERA_INTERP_SMOOTH);
  camera_insert_influence_key(&camera, CAMERA_CHANNEL_FOLLOW, 1.0, 1.f, CAMERA_INTERP_SMOOTH);
  camera_insert_influence_key(&camera, CAMERA_CHANNEL_FOLLOW, 2.0, 1.f, CAMERA_INTERP_SMOOTH);
  camera_insert_influence_key(&camera, CAMERA_CHANNEL_FOLLOW, 3.0, 0.f, CAMERA_INTERP_SMOOTH);
  for (double t = 0.0; t <= 3.0; t += 0.01) {
    const float value = camera_eval_follow(&camera, t);
    assert(value >= 0.f && value <= 1.f);
  }
  near(camera_eval_follow(&camera, 1.5), 1.0);
  near(camera_eval_follow(&camera, 0.5), 0.5);
  near(camera_eval_aim(&camera, 1.5), 0.0);
}

static void editing(void) {
  camera_timeline_t camera;
  camera_timeline_default(&camera);
  camera_pose_t a = at_x(0.f), b = at_x(1.f);
  assert(camera_insert_pose_key(&camera, 1.0, &a, CAMERA_INTERP_SMOOTH) == 0);
  assert(camera_insert_pose_key(&camera, 1.0, &b, CAMERA_INTERP_SMOOTH) == 0); // replaces
  assert(camera.pose_count == 1);
  near(camera.pose_keys[0].pose.eye[0], 1.0);
  assert(camera_insert_pose_key(&camera, 2.0, &a, CAMERA_INTERP_LINEAR) == 1);
  assert(camera_move_key(&camera, CAMERA_CHANNEL_POSE, 1, 0.5) == 0); // reorders
  near(camera.pose_keys[0].time, 0.5);
  assert(camera.pose_keys[0].interp == CAMERA_INTERP_LINEAR);
  assert(camera_move_key(&camera, CAMERA_CHANNEL_POSE, 0, 1.0) == 0); // occupied
  near(camera.pose_keys[0].time, 0.5);
  assert(camera_find_key(&camera, CAMERA_CHANNEL_POSE, 1.02, 0.05) == 1);
  assert(camera_find_key(&camera, CAMERA_CHANNEL_POSE, 1.2, 0.05) == -1);
  *camera_channel_key_interp(&camera, CAMERA_CHANNEL_POSE, 1) = CAMERA_INTERP_HOLD;
  assert(camera.pose_keys[1].interp == CAMERA_INTERP_HOLD);

  camera_insert_time_key(&camera, 0.75, 10.0, CAMERA_INTERP_LINEAR);
  camera_ripple(&camera, 0.75, 2.0);
  near(camera.pose_keys[0].time, 0.5);
  near(camera.pose_keys[1].time, 3.0);
  near(camera.time_keys[0].time, 2.75);
  near(camera_last_key_time(&camera), 3.0);
  // Keys stay at zero seconds or later.
  assert(camera_move_key(&camera, CAMERA_CHANNEL_POSE, 0, -2.0) == 0);
  near(camera.pose_keys[0].time, 0.0);
  camera_pose_t early = at_x(5.f);
  assert(camera_insert_pose_key(&camera, -1.0, &early, CAMERA_INTERP_SMOOTH) == 0);
  near(camera.pose_keys[0].time, 0.0);
  camera_delete_key(&camera, CAMERA_CHANNEL_POSE, 0);
  assert(camera.pose_count == 1);
  near(camera.pose_keys[0].time, 3.0);
}

static void handles(void) {
  camera_timeline_t camera;
  camera_timeline_default(&camera);

  // Easy ease on game time: the game comes to rest at the key and leaves from rest.
  camera_insert_time_key(&camera, 0.0, 0.0, CAMERA_INTERP_SMOOTH);
  camera_insert_time_key(&camera, 1.0, 50.0, CAMERA_INTERP_SMOOTH);
  camera_insert_time_key(&camera, 2.0, 100.0, CAMERA_INTERP_SMOOTH);
  const double before = camera_game_tick(&camera, 1.0, TPS) - camera_game_tick(&camera, 0.99, TPS);
  assert(before > 0.4); // auto: about 1x through the key
  camera_easy_ease(&camera, CAMERA_CHANNEL_TIME, 1);
  const double eased = (camera_game_tick(&camera, 1.001, TPS) - camera_game_tick(&camera, 0.999, TPS)) / 0.002;
  assert(fabs(eased) < 1.0);
  near(camera_game_tick(&camera, 1.0, TPS), 50.0);

  // A dragged handle: aligned keeps both sides on one slope.
  camera_set_ease_handle(&camera, CAMERA_CHANNEL_TIME, 1, 1, 100.f, 0.5f, TPS);
  camera_ease_t ease = camera_curve_key_ease(&camera, CAMERA_CHANNEL_TIME, 1, TPS);
  assert(ease.mode == CAMERA_HANDLE_ALIGNED);
  near(ease.in_slope, 100.0);
  near(ease.out_influence, 0.5);
  camera_set_ease_mode(&camera, CAMERA_CHANNEL_TIME, 1, CAMERA_HANDLE_FREE, TPS);
  camera_set_ease_handle(&camera, CAMERA_CHANNEL_TIME, 1, -1, 10.f, 0.25f, TPS);
  ease = camera_curve_key_ease(&camera, CAMERA_CHANNEL_TIME, 1, TPS);
  near(ease.in_slope, 10.0);
  near(ease.out_slope, 100.0);
  // Auto keys report the handles they compute.
  ease = camera_curve_key_ease(&camera, CAMERA_CHANNEL_TIME, 0, TPS);
  assert(ease.mode == CAMERA_HANDLE_AUTO);
  near(ease.out_influence, 1.0 / 3.0);

  // Spatial handles bend the path between keys without moving the keys.
  camera_timeline_default(&camera);
  camera_pose_t a = at_x(0.f), b = at_x(4.f);
  camera_insert_pose_key(&camera, 0.0, &a, CAMERA_INTERP_LINEAR);
  camera_insert_pose_key(&camera, 1.0, &b, CAMERA_INTERP_LINEAR);
  float point[3];
  camera_path_point(&camera, 0, 0, 0.5, point);
  near(point[1], 0.0);
  const float up[3] = {0.f, 3.f, 0.f};
  camera_set_spatial_handle(&camera, 0, 0, 1, up);
  assert(camera.pose_keys[0].spatial.mode == CAMERA_HANDLE_ALIGNED);
  camera_path_point(&camera, 0, 0, 0.5, point);
  assert(point[1] > 0.5f);
  camera_path_point(&camera, 0, 0, 1.0, point);
  near(point[0], 4.0);
  near(point[1], 0.0);
  // Linear timing moves along the bent path at one speed.
  double speeds[2];
  const double samples[2] = {0.2, 0.7};
  for (int i = 0; i < 2; ++i) {
    const camera_pose_t p = camera_eval_pose(&camera, samples[i]), q = camera_eval_pose(&camera, samples[i] + 0.01);
    speeds[i] = hypot(hypot(q.eye[0] - p.eye[0], q.eye[1] - p.eye[1]), q.eye[2] - p.eye[2]) / 0.01;
  }
  assert(fabs(speeds[0] - speeds[1]) < 0.05 * speeds[0]);

  // Even speed retimes the middle key to where the distance puts it.
  camera_timeline_default(&camera);
  camera_pose_t c0 = at_x(0.f), c1 = at_x(1.f), c2 = at_x(4.f);
  camera_insert_pose_key(&camera, 0.0, &c0, CAMERA_INTERP_SMOOTH);
  camera_insert_pose_key(&camera, 2.0, &c1, CAMERA_INTERP_SMOOTH);
  camera_insert_pose_key(&camera, 4.0, &c2, CAMERA_INTERP_SMOOTH);
  camera_even_speed(&camera);
  near(camera.pose_keys[1].time, 1.0);
  double distances[CAMERA_MAX_POSE_KEYS];
  camera_pose_distances(&camera, distances);
  near(distances[2], 4.0);
  near(camera_curve_value(&camera, CAMERA_CHANNEL_POSE, 4.0, TPS, distances), 4.0);
  near(camera_curve_value(&camera, CAMERA_CHANNEL_POSE, 1.0, TPS, NULL), 1.0);
}

static void timing(void) {
  camera_timeline_t camera;
  camera_timeline_default(&camera);
  camera.view_scale = 100.f;

  // A zoom on the spot is timed like a move: it eases out and never rushes in.
  camera_pose_t wide = at_x(0.f), close = at_x(0.f), away = at_x(10.f);
  close.zoom = away.zoom = 4.f;
  camera_insert_pose_key(&camera, 0.0, &wide, CAMERA_INTERP_SMOOTH);
  camera_insert_pose_key(&camera, 1.0, &close, CAMERA_INTERP_SMOOTH);
  camera_insert_pose_key(&camera, 2.0, &away, CAMERA_INTERP_SMOOTH);
  double previous = 0.0;
  for (double t = 0.01; t <= 1.0; t += 0.01) {
    const double progress = log(camera_eval_pose(&camera, t).zoom) / log(4.0);
    assert(progress >= previous - 1e-9);
    // Slows into the key rather than rushing it: under its average rate there.
    if (t > 0.9) assert((progress - previous) / 0.01 < 1.0);
    previous = progress;
  }

  // A dragged handle on it shapes the zoom instead of snapping it.
  camera_set_ease_handle(&camera, CAMERA_CHANNEL_POSE, 0, 1, 150.f, 0.33f, TPS); // about twice its average
  assert(camera_eval_pose(&camera, 0.06).zoom < 3.f);

  // A slow stretch of game time keeps its own speed up to a faster one.
  camera_timeline_default(&camera);
  camera_insert_time_key(&camera, 0.0, 0.0, CAMERA_INTERP_SMOOTH);
  camera_insert_time_key(&camera, 1.0, 25.0, CAMERA_INTERP_SMOOTH);
  camera_insert_time_key(&camera, 2.0, 125.0, CAMERA_INTERP_SMOOTH);
  near(camera_game_tick(&camera, 0.5, TPS), 12.5);
}

int main(void) {
  timing();
  handles();
  game_time();
  pose();
  influence();
  editing();
  return 0;
}
