param([string]$Image = 'heroeslore-psp-sdkcurrent')
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$dockerfile = Join-Path $projectRoot 'cpp/cmake/Dockerfile.psp'
docker build -t $Image -f $dockerfile (Split-Path -Parent $dockerfile)
if ($LASTEXITCODE -ne 0) { throw 'Docker image build failed' }
docker run --rm --mount "type=bind,source=$projectRoot,target=/src" -w /src $Image sh tools/build_psp_container.sh
if ($LASTEXITCODE -ne 0) { throw 'PSP build failed' }
