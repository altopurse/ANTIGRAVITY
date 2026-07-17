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

## Our situation (checked July 2026)

Microsoft's signing service - now called **Azure Artifact Signing** (formerly
Trusted Signing), $9.99/month - only accepts **individual** developers from
the **USA and Canada**. UK-based *organizations* are eligible, but UK
*individuals* are not. So as a UK individual there are exactly two paths:

## Option A (recommended when revenue justifies it): register a UK Ltd

1. Register a limited company at Companies House (~£50, done online in a day).
   Bonus beyond signing: the company name appears on the certificate (looks
   more professional than a personal name) and gives the app business a
   liability wrapper.
2. Create a paid Azure subscription (pay-as-you-go; free/trial subscriptions
   are rejected). Billing is the full month, not pro-rated.
3. Azure portal: register the `Microsoft.CodeSigning` resource provider on
   the subscription (Subscription -> Resource providers).
4. Create an **Artifact Signing** account: Basic SKU ($9.99/mo), region
   West Europe (endpoint `https://weu.codesigning.azure.net`).
5. On the account's Access control (IAM): assign yourself the
   **Identity Verifier** role (the "New identity validation" button is
   greyed out without it).
6. New identity validation -> **Organization** -> the Ltd's Companies House
   details. Click the email verification link **within 7 days** (it cannot
   be resent; a missed link means starting over).
7. When validation shows Completed: create a **certificate profile**
   (type: Public Trust), then assign yourself the
   **Certificate Profile Signer** role (separate from the verifier role).
8. On the build PC: install the Windows SDK (signtool), the
   `Microsoft.Trusted.Signing.Client` NuGet package (contains
   `Azure.CodeSigning.Dlib.dll`), and the Azure CLI; run `az login`.
9. Create `metadata.json` next to the dlib:
   ```json
   {
     "Endpoint": "https://weu.codesigning.azure.net",
     "CodeSigningAccountName": "<account-name>",
     "CertificateProfileName": "<profile-name>"
   }
   ```
10. Package with signing enabled:
    ```powershell
    $env:CODESIGN_CMD = 'signtool sign /v /fd SHA256 /tr http://timestamp.acs.microsoft.com /td SHA256 /dlib "C:\tools\Azure.CodeSigning.Dlib.dll" /dmdf "C:\tools\metadata.json" {file}'
    powershell -ExecutionPolicy Bypass -File package.ps1
    ```

## Option B (available today as an individual): Certum / SSL.com OV cert

Traditional CAs validate individuals in the UK. **Certum's "Open Source Code
Signing"** product is the budget favourite for indie devs (roughly EUR 70 for
a multi-year cert; despite the name it's fine for freeware/shareware-style
distribution - check their current terms). SSL.com is the pricier
alternative.

- Validation: passport/ID + a short video or notary-style check, done online.
- Delivery: since 2023 keys ship on a USB token or in their cloud signer
  (SimplySign) - not a plain PFX. Follow the CA's signtool guide; their
  command plugs into the same hook:
  ```powershell
  $env:CODESIGN_CMD = '<the CA''s documented signtool command> {file}'
  ```
- Honest trade-off: an OV cert starts with **zero SmartScreen reputation**.
  The warning only fades after enough people download the signed installer
  (weeks to months at low volume). Microsoft-validated identities (Option A)
  gain trust much faster. Signed-but-new is still strictly better than
  unsigned: reputation accrues to the certificate and persists across
  releases, and AV false-positives drop immediately.

## Either way, afterwards

- Submit the signed installer to Microsoft Security Intelligence
  (https://www.microsoft.com/wdsi/filesubmission) to speed up SmartScreen
  reputation.
- Keep signing every release with the same identity - reputation attaches to
  the publisher and compounds.
- **Never commit certificates, tokens or passwords.** Env vars only.
