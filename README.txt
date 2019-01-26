OpenRemoteSlide

huangch


==========================


What is this?
=============

This library is an extension of OpenSlide (you probably should not be here
if you don't know what it is). OpenRemoteSlide is just similar to OpenSlide,
except OpenRemoteSlide can read whole slide images from remote. 

Using the tool, openremoteslide-show-properties, as an example: In OpenSlide
you can read the properties of a whole slide image by:

openslide-show-properties path/to/a/wsi

E.g.:

./openslide-show-properties ./54089012-2e2f-453f-8b88-7f80d6791eb7.svs


In OpenRemoteSlide, you can do this:

openslide-show-properties url/to/a/wsi

E.g.:

./openslide-show-properties https://api.gdc.cancer.gov/legacy/data/54089012-2e2f-453f-8b88-7f80d6791eb7

54089012-2e2f-453f-8b88-7f80d6791eb7 is the UUID of a TCGA whole slide image.

For the other details, please see README-OpenSlide.txt

Good luck!
