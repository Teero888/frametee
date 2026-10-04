TrackMania United Forever support
=================================

The TMUF game module is included with FrameTee, but the copyrighted game data
is not redistributed. FrameTee first checks this directory for `Packs` and
`GameData`. If they are missing, it automatically looks for TrackMania United
Forever (2.11.26) in Steam on Windows and Linux, including additional Steam
libraries. You do not need to copy files from a Steam installation.

For other installations, copy the `Packs` and `GameData` directories into
this directory before opening a track.

The resulting layout must be:

    data/games/tmuf/Packs/
    data/games/tmuf/GameData/

Players' own skins (the ones a replay names but GameData does not have) are
looked for as the game does, in a TrackMania documents folder: put them in
data/games/tmuf/Documents/ (e.g. Documents/Skins/Vehicles/StadiumCar/X.zip);
~/Documents/TrackMania/ and Wine's are searched too.
