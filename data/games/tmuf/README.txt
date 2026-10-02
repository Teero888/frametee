TrackMania United Forever support
=================================

The TMUF game module is included with FrameTee, but the copyrighted game data
is not redistributed. Copy the `Packs` and `GameData` directories from a
TrackMania United Forever installation (2.11.26) into this directory before
opening a track.

The resulting layout must be:

    data/games/tmuf/Packs/
    data/games/tmuf/GameData/

Players' own skins (the ones a replay names but GameData does not have) are
looked for as the game does, in a TrackMania documents folder: put them in
data/games/tmuf/Documents/ (e.g. Documents/Skins/Vehicles/StadiumCar/X.zip);
~/Documents/TrackMania/ and Wine's are searched too.

What the module does with them:

- Challenges (.Challenge.Gbx) open as levels; a replay (.Replay.Gbx) opened
  as a level loads its map with the car, seed and laps it was driven with.
- Replays import as recordings: the ghost's inputs play back exactly, and
  input snippets placed after them carry on from where the run was.
- Runs export as .Replay.Gbx (TrackMania > Export Replay...), up to the
  finish or else the playhead, which the game plays and its
  validator accepts.
- The view draws the track, the sky of the map's mood and the car with its
  animated wheels, suspension and pilot, from the game's own files. The
  "Race" camera is the game's first race camera; "Orbit" turns around the car.
