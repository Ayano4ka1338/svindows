================================================================================
                   INFORMATION REGARDING POSTSCRIPT PRINTERS
================================================================================

You have selected one of the following printers:

    Apple LaserWriter                 Apple LaserWriter Plus
    Dataproducts LZR 2665             Digital LN03R ScriptPrinter
    Digital LPS PrintServer 40        IBM Personal Pageprinter
    Linotronic 100/300/500            QMS-PS 800
    QMS-PS 800 Plus                   TI OmniLaser 2108
    TI OmniLaser 2115 Wang LCS15      Wang LCS15 FontPlus

These printers are known as "PostScript  Printers."  Your printer has
been installed; however, it has not yet been configured properly.  To
properly configure your PostScript printer, use the Control Panel.
For information on how to do this, see "Configuring Your System" in
Chapter 7, "Using Control Panel", of the  Microsoft  Windows User's
Guide.  Be sure to properly set up your communications port.



Printing Faster With a Postscript Printer:

Each time you print from Windows to a PostScript printer, Windows
first  prepares  the  printer by copying the file, PSPREP.TXT, to
it.  Since copying this  file  to  the  printer  takes  about  30
seconds,  and  the  printer only needs this information once each
time it is turned on, you can save time by telling Windows not to
copy PSPREP.TXT to the printer and then doing it yourself.

NOTE:  Do NOT do this if you will be  creating  PostScript  print
files to send to a service bureau for printing.

If you want to do this, first use Control Panel to configure your
printer.   Then  use  Notepad  to  edit  your  WIN.INI  file.  In
WIN.INI,  search   for   the  keyword  describing  your  printer,
( e.g. [PostScript,COM1] ).  Below  this  keyword, add  the  line
"Header Downloaded=yes."   This will  tell  Windows  not  to copy
PSPREP.TXT to the printer.

Now, each time you turn on your printer, you must copy the  file,
PSPREP.TXT,   located   on   the   "Utilities   Disk,"   to   the
communications port which connects your printer to your computer.
For  instance,  assume you have copied the file, PSPREP.TXT, from
the "Utilities Disk" to the  C:\WINDOWS directory  on  your  hard
disk,   and   that   your  printer  is  connected  to  the  COM1:
communications port.  Then, AFTER you turn on  your  printer  and
start  Windows,  you would need to use the File Copy command from
MS-DOS Executive to copy  PSPREP.TXT  from  C:\WINDOWS  to COM1:.
Likewise, before starting  Windows, you could enter the following
command at the DOS prompt:

           COPY C:\WINDOWS\PSPREP.TXT COM1:

You  could  add the above DOS command to your  AUTOEXEC.BAT  file
so that  your  computer  automatically  copies PSPREP.TXT to your
printer each time you turn it on. If you do this, BE SURE to turn
on your printer  BEFORE you turn on your computer or your printer
will not be properly prepared for  printing.  If you do forget to
turn on your  printer  first,  simply  issue the above command to
prepare your printer.




APPLE LASERWRITER OR APPLE LASERWRITER PLUS

Printing Over a Local Area Network:

Typically Windows sends your  printouts to  a  temporary  holding
space,  called  the Spooler, until the printer becomes available.
Some Local Area Networks have their own Spooler.  If  your  Apple
printer  is  connected  to a Local Area Network which has its own
Spooler, you may  wish  to  disable  the  Windows  Spooler.   For
information  on how to disable the Spooler, see "Printing Without
the Spooler" in Appendix A, "Customizing Your WIN.INI  File,"  in
the Microsoft Windows User's Guide.

If you use a Local Area Network Spooler rather than  the  Windows
Spooler,  you  may  need  to  configure your Apple printer to use
hardware handshaking;  this  is  typical  when  using  a  printer
connected to a Local Area Network.

To configure your Apple printer for hardware handshaking, use the
Control  Panel  to  change  your  printer's communication port to
"Hardware" for the "Handshaking" option.   For  more  information
see  "Setting  Up  a  Communications  Port"  in Chapter 7, "Using
Control Panel," in the Microsoft Windows User's Guide.  Also, you
must  copy  the  file, HARDWARE.TXT, from the "Utilities Disk" to
your printer's communication  port.   For  example,  assume  your
printer  is  connected  to  the  COM1:  serial port, and that the
"Utilities Disk" is in your computer's A: disk drive.  You  would
type the command

                    COPY A:HARDWARE.TXT COM1:

at the DOS prompt, and then type ENTER.  You must do this  before
you  start Microsoft Windows.  You need only copy HARDWARE.TXT to
your printer once;  after you have done so,  the information will
be permanently stored  in your printer,  even if you turn off the
power.

If you have  trouble  printing  after  setting  up  for  hardware
handshaking,  try  switching  to  software  handshaking.   Choose
"None"  for  your  printer's  communication  port's   "Handshake"
option.  Then copy SOFTWARE.TXT from the "Utilities Disk" to your
printer's port.


If you still have trouble printing, you may have a faulty  serial
port, or you may have an older model of the Apple LaserWriter. In
order to resolve the latter problem, you may need to upgrade your
LaserWriter.  In either case, contact your printer dealer.

Other Problems Solved by HARDWARE.TXT and SOFTWARE.TXT:

By default,  your  printer  will  abandon your printout within 30 
seconds if some problem occurs while printing ( e.g. your printer
runs out of paper ).  Copying either HARDWARE.TXT or SOFTWARE.TXT
( whichever is appropriate ) to your printer's port will increase
this time to five minutes.   We  recommend  that  you change this
timeout to five  minutes by  copying  one of these  files to your
printer's port.

If you are only getting the first few pages of a printout, it may
be because  your  printer is connected  to  a  device  which does
hardware handshaking. Try setting up hardware handshaking per the
instructions above.



Converting Downloadable Fonts:

If  you  are  using downloadable  fonts  which have corresponding
screen  fonts  ( such as Adobe's ),  then you may need to use the
NEWFON.EXE utility to convert these screen fonts to a new format.
See the file, README.TXT, on the Write Disk for more details.

Printing ITC Zapf Dingbats:

With the LaserWriter  Plus,  you can  select the ITC Zapf Dingbat
fonts,  ZapfDingbats  (i.e.  a number of special symbols).   When
using these fonts, the screen will display normal characters:  no
font is  available  for  viewing  the  dingbat  characters on the
Windows screen.   However, when  the  document  is  printed,  the
dingbat characters will be printed instead of normal characters.







Printing Complicated Print Jobs:

The Apple LaserWriter and LaserWriter Plus may spend a long  time
"computing"  the image of very complicated printouts.   When this
happens, the Spooler occasionally alerts you that the printer  is
not  responding  with  a  dialog  box asking if you would like to
cancel the printout.   If this happens, choose the "Retry" button
and your printer will  continue  "computing."   If you need to do
this  often,  you may  want to  adjust  the  "Transmission retry"
timeout with the Control Panel.  For more information on changing
the "Transmission retry", see "Setting Up Printers" in Chapter 7,
"Using the Control Panel," in the Microsoft Windows User's Guide.

LINOTRONIC IMAGESETTER:

When configuring your Linotronic printer,  be  sure  to  set  the
"Graphics  resolution:"  option  to  the resolution of your model
(for  example,  1270  dots  per  inch).    For   information   on
configuring your printer, see "Setting Up Printers" in Chapter 7,
"Using the Control Panel," in the Microsoft Windows User's Guide.


IBM PERSONAL PAGEPRINTER:

In order to use the IBM Personal Pageprinter, you must set up the
postscript  printer  driver to use the EPT: port.  If you did not
do this during Setup,  then  you  may  use  the  "Connections..."
command from the "Setup" menu of the control panel.  Special EPT:
port  software is  provided  with  your IBM  printer.   For  more
information, see your IBM Personal Pageprinter documentation.

TI OMNILASER:

When printing to a TI OmniLaser, you may experience problems such
as garbled output or messages such  as  "Can't write to printer."
These  problems only occur when the printer is connected directly
to the computer via its parallel port. If you get garbled output,
then connect your printer to the computer via  a serial port.  If
you get messages such as  "Can't write to printer,"  then  either
connect your printer to the computer via a serial port, or simply
press "Retry" when Windows says this.
================================================================================
                              END OF READMEPS.TXT
================================================================================
