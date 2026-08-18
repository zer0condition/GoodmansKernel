Goodmans deploy folder

After running build.cmd from the repo root, this directory contains
everything needed to install and run Goodmans on a test VM.

layout after build:
  deploy\
    Goodmans.sys          signed driver (test-cert)
    Goodmans.inf          driver inf
    GoodmansTest.cer      test code-signing cert
    GoodmansTest.pfx      pfx used for signing (password: goodmans)
    gen_cert.cmd          creates cer+pfx (run once, elevated)
    install.cmd           imports cert + sc create + sc start
    uninstall.cmd         sc stop + sc delete
    README.txt            this file
    gui\
      goodmans-gui.exe    debug GUI (Qt6)
      Qt6*.dll            + platform + tls + imageformats plugins
      toolkit.wasm        toolkit guest for the Explorer tab
    samples\
      sample_guest.wasm   demo host-import roundtrips
      ffi_demo.wasm       direct nt/hal export calls via host_call
      pslist_dumper.wasm  walks PsLoadedModuleList
      handle_stripper.wasm  ObDereferenceObject demo
      infinity_hook.wasm  full IH port in wasm
    features\
      toolkit.wasm
      process_tracer.wasm

workflow on the target VM

1. enable test signing (once, then reboot):
     bcdedit /set testsigning on

2. copy the whole `deploy\` folder to the VM. from an elevated cmd here:
     gen_cert.cmd        (first-time setup, creates the cer+pfx)
     install.cmd         (imports cert + sc create/start Goodmans)

3. run the GUI:
     gui\goodmans-gui.exe
   the toolkit.wasm loads automatically; the Explorer tab wakes up.
   load any sample from Workbench, Browse, samples\*.wasm

4. teardown:
     uninstall.cmd

common errors

sc start returns 577    driver isnt signed with a trusted cert.
                        re-run install.cmd. verify cert landed in
                        Cert:\LocalMachine\Root and TrustedPublisher.

sc start returns 1275   testsigning off, or HVCI blocking. verify:
                          bcdedit /enum {current}
                        testsigning should say Yes.

CreateFile fails 2      driver started but DriverEntry bailed. open the
                        GUI Log tab or DbgView filtered on [goodmans].

bugcheck on start       paste the bugcheck code + parameters into an issue.
