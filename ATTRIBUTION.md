# Data attribution

Fovea's code does not include map data. Terrain databases are built from
public sources by `tools/fetch-sources.py` and `fovea-compiler`; the source
lists are in `data/sources/`. The data stays its providers'; credit them
wherever a database built from it is shown or shared.

| Data | Provider | Terms | Credit |
|---|---|---|---|
| 3DEP 1 m lidar DEMs and 1/3 arc-second DEM | U.S. Geological Survey, 3D Elevation Program | U.S. Government work, public domain; acknowledgement requested ([USGS data policy](https://www.usgs.gov/information-policies-and-instructions/copyrights-and-credits)) | "Elevation data: U.S. Geological Survey, 3D Elevation Program" |
| NAIP aerial imagery | U.S. Department of Agriculture, Farm Production and Conservation Business Center (National Agriculture Imagery Program) | U.S. Government work, public domain | "Imagery: USDA National Agriculture Imagery Program" |
| GEOID18 | NOAA National Geodetic Survey | U.S. Government work, public domain | "GEOID18: NOAA National Geodetic Survey" |
| Airport and runway data (`data/airports/`) | Federal Aviation Administration: runway positions as published by AirNav; runways and their lights from the FAA's NASR airport data (28-day subscription), checked by `tools/check-airports.py` | U.S. Government work, public domain | "Runways: Federal Aviation Administration" |
| The stars (`libs/render/src/vk/star_catalog.inc`, made by `tools/fetch-stars.py`) | Yale Bright Star Catalogue, 5th revised edition (Hoffleit & Warren 1991), from NASA's Astronomical Data Center, as the Harvard-Smithsonian Center for Astrophysics distributes it | Distributed freely for research and use; no licence is stated, and citing it is the convention | "Stars: Yale Bright Star Catalogue, 5th ed. (Hoffleit & Warren 1991)" |
| Sun and moon positions the tests check against (`tests/unit/test_celestial.cpp`) | NASA Jet Propulsion Laboratory, Horizons system | U.S. Government work | "Ephemerides: JPL Horizons" |

Hosting, not ownership: NAIP files are fetched from the Microsoft Planetary
Computer, GEOID18 from the PROJ project's data server (cdn.proj.org).

Check each provider's current terms before publishing a database: the links
above are where they keep them.
