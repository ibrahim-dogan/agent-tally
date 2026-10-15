# OpenSCAD with a virtual X server so PNG previews render headless.
FROM openscad/openscad:dev
RUN apt-get update && apt-get install -y --no-install-recommends xvfb xauth fonts-liberation && rm -rf /var/lib/apt/lists/*
