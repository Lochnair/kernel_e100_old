node {
	docker.image('lochnair/octeon-buildenv:latest').inside {
		stage 'Clean workspace'

		sh 'git reset --hard'
		sh 'git clean -fX'

		stage 'Build'

		sh 'make -j5 ARCH=mips CROSS_COMPILE=mips64-octeon-linux- vmlinux modules'

		stage 'Archive kernel image'

		sh 'mv -v vmlinux vmlinux.64'
		sh 'tar cvjf e100-kernel.tar.bz2 vmlinux.64'
		archiveArtifacts artifacts: 'e100-kernel.tar.bz2', fingerprint: true, onlyIfSuccessful: true

		stage 'Archive kernel modules'

		sh 'make ARCH=mips CROSS_COMPILE=mips64-octeon-linux- INSTALL_MOD_PATH=destdir modules_install'
		sh 'tar cvjf e100-modules.tar.bz2 -C destdir .'
		archiveArtifacts artifacts: 'e100-modules.tar.bz2', fingerprint: true, onlyIfSuccessful: true

		stage 'Prepare for out-of-tree builds'
		def extWorkspace = exwsAllocate 'diskpool1'
		def exwsPath = extWorkspace.getCompleteWorkspacePath()
		sh """
			mkdir -p "${exwsPath}"
			install -Dt "${exwsPath}" -m644 Makefile .config Module.symvers
			install -Dt "${exwsPath}"/kernel -m644 kernel/Makefile

			mkdir "${exwsPath}"/.tmp_versions

			cp -t "${exwsPath}" -a include scripts

			install -Dt "${exwsPath}"/arch/mips -m644 arch/mips/Makefile
			install -Dt "${exwsPath}"/arch/mips/kernel -m644 arch/mips/kernel/asm-offsets.s

			cp -t "${exwsPath}"/arch/mips -a arch/mips/include

			find . -name 'Kconfig*' -exec install -Dm644 {} "${exwsPath}/{}" '\';

			install -Dt "${exwsPath}"/tools/objtool tools/objtool/objtool

			find -L "${exwsPath}" -type l -printf 'Removing %P\n' -delete
		"""
	}
}
