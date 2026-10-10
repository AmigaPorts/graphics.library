# Copyright © 2026 Marlon Beijer <marlon@amigadev.com>
# https://github.com/mbeijer
#
# This Source Code Form is subject to the terms of the
# Mozilla Public License, v. 2.0. If a copy of the MPL was not distributed
# with this file, You can obtain one at http://mozilla.org/MPL/2.0/.

function(lha target archive file path outputfile)

	find_program(LHA lha REQUIRED)

	add_custom_command(
			OUTPUT ${path}/${outputfile}
			COMMAND ${LHA} -x ${archive} ${file} -w ${path} -i
			DEPENDS ${archive}
			COMMENT "Extracting ${outputfile} from ${archive}."
			WORKING_DIRECTORY ${path}
			VERBATIM
	)

endfunction(lha)