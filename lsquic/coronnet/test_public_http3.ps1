#requires -Version 5.1

[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [ValidateScript({ Test-Path -LiteralPath $_ -PathType Leaf })]
    [string]$Client,

    [ValidateRange(1, 65535)]
    [int]$Port = 443,

    [ValidateNotNullOrEmpty()]
    [string[]]$ServerName = @(
        'tv.puui.qpic.cn',
        'pbaccess.video.qq.com',
        'www.zhihu.com',
        'www.xiaohongshu.com'
    )
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$Client = (Resolve-Path -LiteralPath $Client).Path

if ($null -eq (Get-Command Resolve-DnsName -ErrorAction SilentlyContinue)) {
    throw 'Resolve-DnsName is required to run the public HTTP/3 test.'
}

function Test-PublicHttp3Server {
    param(
        [Parameter(Mandatory)][string]$Name
    )

    $addresses = @(
        Resolve-DnsName -Name $Name -Type A -DnsOnly |
            Where-Object Type -EQ 'A' |
            Select-Object -ExpandProperty IPAddress -Unique
    )
    if ($addresses.Count -eq 0) {
        throw "No IPv4 address was resolved for $Name."
    }

    foreach ($address in $addresses) {
        Write-Host "Testing ${Name}:${Port} at $address"
        & $Client '--handshake' $address ([string]$Port) $Name
        if ($LASTEXITCODE -eq 0) {
            Write-Host "PASS: $Name ($address)"
            return
        }
        Write-Warning "Handshake failed for $Name at $address with exit code $LASTEXITCODE."
    }

    throw "HTTP/3 handshake failed for every resolved address of $Name."
}

foreach ($name in $ServerName) {
    Test-PublicHttp3Server -Name $name
}

Write-Host "PASS: HTTP/3 handshakes completed for $($ServerName.Count) public servers."
