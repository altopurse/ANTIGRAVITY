# Code signing the installer

Unsigned installers hit the blue SmartScreen wall ("Windows protected your
PC") on every single download. Signing removes that wall over time and stops
antivirus false-positives. It is the single highest-conversion-impact change
available to this project.

`package.ps1` already contains the signing hook - it signs both
`voice-changer.exe` and `AntigravityVoiceEngine-Setup.exe` automatically when
the environment variables below are set, and computes the anti-tamper
`EXPECTED_HASH` *after* signing so it stays correct. Nothing else needs to
change; you only need a certificate.

## Recommended: Azure Trusted Signing (~$9.99/month)

Microsoft's own signing service. Cheapest option, and SmartScreen trusts its
certificates quickly (often immediately, because Microsoft vouches for the
identity check).

1. Create an Azure account at https://azure.microsoft.com (needs a card).
2. In the Azure portal, create a **Trusted Signing** resource
   (Basic tier, $9.99/month). Regions: pick West Europe.
3. Complete **identity validation** ("Individual" works for a sole developer -
   you verify with government ID; a company registration works too).
4. Create a **certificate profile** (type: Public Trust).
5. Install the tooling on the build PC:
   - Windows SDK (for signtool): `winget install Microsoft.WindowsSDK.10.0.26100`
   - The dlib: `dotnet tool` or NuGet package `Microsoft.Trusted.Signing.Client`
     (download and note the path to `Azure.CodeSigning.Dlib.dll`).
6. Create `metadata.json` next to the dlib:
   ```json
   {
     "Endpoint": "https://weu.codesigning.azure.net",
     "CodeSigningAccountName": "<your-account-name>",
     "CertificateProfileName": "<your-profile-name>"
   }
   ```
7. Sign in once with `az login` (Azure CLI), then set for packaging:
   ```powershell
   $env:CODESIGN_CMD = 'signtool sign /v /fd SHA256 /tr http://timestamp.acs.microsoft.com /td SHA256 /dlib "C:\tools\Azure.CodeSigning.Dlib.dll" /dmdf "C:\tools\metadata.json" {file}'
   powershell -ExecutionPolicy Bypass -File package.ps1
   ```

## Alternative: classic OV certificate (PFX file)

Buy an OV code-signing certificate from a CA (Certum is the budget option for
individuals, ~€70/three years for an "Open Source" cert; Sectigo/DigiCert cost
more). Since 2023 these ship on hardware tokens or cloud HSMs rather than
plain PFX files, so follow the CA's signtool instructions - or if you do have
a PFX:

```powershell
$env:CODESIGN_PFX = "C:\secure\antigravity.pfx"
$env:CODESIGN_PFX_PASSWORD = "..."
powershell -ExecutionPolicy Bypass -File package.ps1
```

## Notes

- **Never commit the certificate or password.** They live only in env vars /
  a secure folder.
- OV certs (unlike Azure Trusted Signing or EV) start with zero SmartScreen
  reputation - the warning fades only after enough downloads. Trusted Signing
  is both cheaper and faster to trust; prefer it.
- After the first signed release, keep signing every release with the same
  identity - reputation attaches to the publisher.
