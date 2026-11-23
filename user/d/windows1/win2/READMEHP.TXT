================================================================================
                       INFORMATION REGARDING PCL PRINTERS
================================================================================

You have selected one of the following printers:

	Apricot Laser              HP LaserJet
	HP LaserJet Plus           HP LaserJet 500+
	HP LaserJet Series II      HP LaserJet 2000
	Kyocera F-1010 Laser       QuadLaser I

These printers  are  known as "PCL" printers.  Your  PCL  printer has
been installed; however, it has not yet been configured properly.  To
properly configure your PCL printer, use the Control Panel.   For more
information  on  how to do  this,  see  "Configuring Your System"  in
Chapter 7,  "Using Control Panel",  of  the  Microsoft Windows User's
Guide.  Be sure to select the proper font cartridge for your printer;
if you do not, your font options may be limited by some applications.
Also, be sure to properly set up your communications port.



Using Soft Fonts With Hewlett Packard Laser Printers

The following  describes  how  to  use soft  fonts  with  Hewlett
Packard  Laser  Printers.   To  use soft fonts, your printer must
have the proper hardware.  The HP LaserJet will not support  soft
fonts.   The  HP  LaserJet+, HP LaserJet II, and HP LaserJet 2000
all support soft fonts.  If you have a printer which emulates  an
HP  Laser  Printer and wish to use soft fonts, you should contact
your printer dealer or manufacturer to determine if your  printer
supports  soft  fonts.  We recommend that you use soft fonts only
if you have a fixed disk; do not  use  soft  fonts  with  a  dual
floppy system.

The style and size of the letters,  numbers,  and  other  symbols
used  by  Windows  and  your Hewlett Packard Laser Printer depend
upon the "font" being used; a "font" describes the style and size
of  these  letters.   A  "soft  font" is a file containing a font
description.  These soft fonts may be copied  to  your  HP  Laser
Printer giving you a greater number of alphabet styles and sizes.



You must purchase soft fonts separately; they do  not  come  with
the Windows package.

Typically the maker of your soft fonts will  provide  a  complete
set  of instructions and utilities for installing your fonts.  If
you plan to use the soft fonts with a non-Windows application  or
if  you  want to permanently install these fonts on your printer,
first  follow  the maker's  installation  instructions  and  then
follow the  instructions  below.   If  you  will  be  permanently
installing  these  fonts on your  printer,  make a note of the ID
numbers for each font; you will need these numbers later when you
edit  WIN.INI.   Please  refer  questions  regarding  non-Windows
installation to the maker of your soft fonts.

If you plan to use your soft fonts with Microsoft  Windows  only,
then  you need only follow the installation instructions outlined
below.





Soft Font Installation Instructions for Microsoft Windows

1.  Create a sub-directory named PCLPFM in the Windows directory.
For  example, if you used the default directory, C:\WINDOWS, when
you did your Windows Setup, then this  new  directory  should  be
named  C:\WINDOWS\PCLPFM.  This directory  will  be used to store
PCLPFM.EXE ( the Windows soft font installation  program  ),  and
your soft font files.

2.  Copy  PCLPFM.EXE  from  the  Fonts  Disk to the newly created
PCLPFM sub-directory.

3.   In  your  soft  font  package,  you  will  find  a  diskette
containing  soft font files ( see the documentation provided with
your soft font package to determine which files are the soft font
files  ).   You  may  use  some or all of these soft fonts.  Copy
those soft font files you wish to use from this diskette  to  the
PCLPFM  sub-directory.   Make  a note of the  filenames you copy;
these will be important in later steps.



4.  Run the program PCLPFM.EXE.  You may run  this  program  from
Windows,  or  simply  from  DOS.   To run from the Windows MS-DOS
Executive, double-click on PCLPFM.EXE.  To run from  DOS,  simply
type PCLPFM at the DOS prompt and then press the ENTER key.

5.  PCLPFM will  prompt you to enter a filename.  When  prompted,
type  a  single  soft font filename, and press the ENTER key.  If
you wish to enter more than one soft font filename,  you may  use
the  "wildcard" characters, '?' and '*' ( for more information on
the use of '?' and '*', see "global filename characters" in  your
DOS manual ).

6.  Using the soft font filename,  PCLPFM will now create  a  new
filename  having  a ".PFM" file extension.  When creating these
new names, PCLPFM replaces the last character of  the  soft  font
filename  with  a P (for Portrait) or an L (for Landscape), and
changes the filename extension to ".PFM."

For example, from a soft font  file  named  HV10B#R8.SFP,  PCLPFM
will create a new file named HV10B#RP.PFM.


If PCLPFM finds that the newly  created  filename is the  same as
an  already  existing file, then it will give you the opportunity
to either replace the existing file, or change the newly  created
filename to something  different.  In this situation, type 'Y' to
replace the existing file, or 'N' to change the newly created PFM
filename.

If a single font file was specified, then PCLPFM  gives  you  the
opportunity  to change the newly created PFM filename.   However,
if  multiple font filenames  were  specified,  then  PCLPFM  will
simply  display  each newly created  PFM filename to you one at a
time.  In this situation, press the ENTER key to accept these new
filenames.  If you wish to end the process, press CONTROL+C (hold
down the CONTROL key and press 'C').

7.  Once PCLPFM has displayed each new PFM  filename,  it asks if
you would like to save the necessary soft font  information  in a
file called APPNDWIN.INI.  You should enter 'Y'  unless  you wish
to end the process.



Upon entering 'Y', PCLPFM will  create  the  file,  APPDNWIN.INI.
The  text saved in this file should be added to your WIN.INI file
so that Windows will know how to  take  advantage  of  your  soft
fonts (for more information on the WIN.INI file,  see Appendix A,
"Customizing Your WIN.INI File" in the Microsoft  Windows  User's
Guide).

8.  The last step is to insert the contents of  the  APPNDWIN.INI
file into the proper place in your WIN.INI file.

    A)  From the Windows MS-DOS Executive, double-click on
        APPNDWIN.INI.  This will bring up the APPNDWIN.INI
        file in Notepad.

    B)  Choose "Select all" from  Notepad's Edit menu, and
        then choose "Copy" from Notepad's Edit menu.  This
        will copy  the entire contents of the APPNDWIN.INI
        file to the Clipboard.

    C)  Switch  back to the  MS-DOS  Executive  window and
        double-click  on  WIN.INI.  This will bring up the
        WIN.INI file in Notepad.

    D)  In WIN.INI, search for the printer section related
        to  your  printer.  The  section header  for  this
        section should look something like:

        [HPPCL,LPT1]

    E)  Now insert the text from APPNDWIN.INI into WIN.INI
        starting  on  the  line  just  after this  section
        header.  To do this, move the cursor to the end of
        the  section header, [HPPCL,LPT1], and then choose
        "Paste" from Notepad's Edit menu.

    F)  Select  and  delete the small  box (an end-of-file
        marker), if there is one, from the end of the soft
        font information you've just copied.

        If  you  used a  wildcard when running PCLPFM, you
        may have created  more PFM files than you actually
        wanted.  Check the  new lines in your WIN.INI file
        and delete the  entire line for any PFM  file that
        you do not want.

        For lines  containing fonts you  want  permanently
        installed, delete everything from the comma to the
        end of the line.  Then edit the font ID number, if
        necessary, to  match the  number  loaded  into the
        printer. Check to see that all font ID numbers are
        unique -- they need not be consecutive.

    G)  To  finish  up,  save  the  new  WIN.INI  file  by
        choosing the "Save" command  from  Notepad's  File
        menu.  Then  close  both  copies of Notepad, close
        Windows, and re-start Windows.

Printer Setup for Soft Fonts

Use the Control Panel to set up your HP Laser Printer. Be sure to
select  the  proper  printer;  if  you select "HP LaserJet"  (the
default printer),  then your soft  fonts  will  not  be  properly
utilized.   If  you  plan  to  use  only soft fonts, set the font
cartridge option to "None".  If you plan to use a font  cartridge
in  addition  to  your  soft  fonts, then you should set the font
cartridge option to the corresponding font cartridge name.

Selecting Soft Fonts from a Windows Application

In a Windows application  which  allows  modification of the font
( refer  to  your application's  documentation for an explanation
of how to change the font ), the list of available  fonts  should
reflect  your  new  soft  fonts  once  they  have  been  properly
installed.











================================================================================
                               END OF READMEHP.TXT
================================================================================
