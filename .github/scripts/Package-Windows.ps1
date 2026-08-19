[CmdletBinding()]
param(
    [ValidateSet('x64')]
    [string] $Target = 'x64',
    [ValidateSet('Debug', 'RelWithDebInfo', 'Release', 'MinSizeRel')]
    [string] $Configuration = 'RelWithDebInfo',
    [switch] $BuildInstaller
)

$ErrorActionPreference = 'Stop'

if ( $DebugPreference -eq 'Continue' ) {
    $VerbosePreference = 'Continue'
    $InformationPreference = 'Continue'
}

if ( $env:CI -eq $null ) {
    throw "Package-Windows.ps1 requires CI environment"
}

if ( ! ( [System.Environment]::Is64BitOperatingSystem ) ) {
    throw "Packaging script requires a 64-bit system to build and run."
}

if ( $PSVersionTable.PSVersion -lt '7.2.0' ) {
    Write-Warning 'The packaging script requires PowerShell Core 7. Install or upgrade your PowerShell version: https://aka.ms/pscore6'
    exit 2
}

function Package {
    trap {
        Write-Error $_
        exit 2
    }

    $ScriptHome = $PSScriptRoot
    $ProjectRoot = Resolve-Path -Path "$PSScriptRoot/../.."
    $BuildSpecFile = "${ProjectRoot}/buildspec.json"

    $UtilityFunctions = Get-ChildItem -Path $PSScriptRoot/utils.pwsh/*.ps1 -Recurse

    foreach( $Utility in $UtilityFunctions ) {
        Write-Debug "Loading $($Utility.FullName)"
        . $Utility.FullName
    }

    $BuildSpec = Get-Content -Path ${BuildSpecFile} -Raw | ConvertFrom-Json
    $ProductName = $BuildSpec.name
    $ProductVersion = $BuildSpec.version

    $OutputName = "${ProductName}-${ProductVersion}-windows-${Target}"

    $RemoveArgs = @{
        ErrorAction = 'SilentlyContinue'
        Path = @(
            "${ProjectRoot}/release/${ProductName}-*-windows-*.zip"
            "${ProjectRoot}/release/${ProductName}-*-windows-*.exe"
        )
    }

    Remove-Item @RemoveArgs

    Log-Group "Archiving ${ProductName}..."
    $CompressArgs = @{
        Path = (Get-ChildItem -Path "${ProjectRoot}/release/${Configuration}" -Exclude "${OutputName}*.*")
        CompressionLevel = 'Optimal'
        DestinationPath = "${ProjectRoot}/release/${OutputName}.zip"
        Verbose = ($Env:CI -ne $null)
    }
    Compress-Archive -Force @CompressArgs
    Log-Group

    if ( $BuildInstaller ) {
        # CMake generates the Inno Setup script from cmake/windows/resources/installer-Windows.iss.in
        $IsccFile = "${ProjectRoot}/build_${Target}/installer-Windows.generated.iss"

        if ( ! ( Test-Path -Path $IsccFile ) ) {
            throw 'Inno Setup install script not found. Run the build script or the CMake build and install procedures first.'
        }

        $Iscc = Get-Command iscc -ErrorAction SilentlyContinue
        if ( $Iscc ) {
            $IsccPath = $Iscc.Source
        } elseif ( Test-Path "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe" ) {
            $IsccPath = "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe"
        } else {
            throw 'Inno Setup 6 (iscc) not found. Install it or use a runner image that ships it.'
        }

        Log-Group "Creating installer ${ProductName}..."

        Push-Location -Stack BuildTemp
        try {
            Ensure-Location -Path "${ProjectRoot}/release"

            # Stage the installed plugin tree at release/Package — the .iss
            # references it by an absolute path baked in at CMake configure time.
            Copy-Item -Path ${Configuration} -Destination Package -Recurse

            Invoke-External $IsccPath ${IsccFile} /O"${ProjectRoot}/release" /F"${OutputName}-Installer"
        } finally {
            Remove-Item -Path Package -Recurse -ErrorAction SilentlyContinue
            Pop-Location -Stack BuildTemp
        }

        Log-Group
    }
}

Package
