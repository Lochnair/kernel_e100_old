pipeline {
    agent {
        docker { image 'lochnair/octeon-buildenv:latest' }
    }
    
    stages {
        stage('Clean') {
            steps {
               sh 'git reset --hard'
               sh 'git clean -fX'
            }
        }

        stage('Build') {
            steps {
                sh 'make -j5 ARCH=mips CROSS_COMPILE=mips64-octeon-linux- vmlinux modules'
            }
        }
        
        stage('Archive kernel image') {
            steps {
                sh 'mv -v vmlinux vmlinux.64'
                sh 'tar cvjf e100-kernel.tar.bz2 vmlinux.64'
                archiveArtifacts artifacts: 'e100-kernel.tar.bz2', fingerprint: true, onlyIfSuccessful: true
            }
        }
        
        stage('Archive kernel modules') {
            steps {
                sh 'make ARCH=mips CROSS_COMPILE=mips64-octeon-linux- INSTALL_MOD_PATH=destdir modules_install'
                sh 'tar cvjf e100-modules.tar.bz2 -C destdir .'
                archiveArtifacts artifacts: 'e100-modules.tar.bz2', fingerprint: true, onlyIfSuccessful: true
            }
        }
        
        stage('Prepare for out-of-tree builds') {
            def extWorkspace = exwsAllocate 'diskpool1'
            steps {
                sh """
                    mkdir -p "${extWorkspace.getCompleteWorkspacePath()}"
                    install -Dt "${extWorkspace.getCompleteWorkspacePath()}" -m644 Makefile .config Module.symvers
                    install -Dt "${extWorkspace.getCompleteWorkspacePath()}"/kernel -m644 kernel/Makefile
                    
                    mkdir "${extWorkspace.getCompleteWorkspacePath()}"/.tmp_versions
                    
                    cp -t "${extWorkspace.getCompleteWorkspacePath()}" -a include scripts
                    
                    install -Dt "${extWorkspace.getCompleteWorkspacePath()}"/arch/mips -m644 arch/mips/Makefile
                    install -Dt "${extWorkspace.getCompleteWorkspacePath()}"/arch/mips/kernel -m644 arch/mips/kernel/asm-offsets.s
                    
                    cp -t "${extWorkspace.getCompleteWorkspacePath()}"/arch/mips -a arch/mips/include
                    
                    find . -name 'Kconfig*' -exec install -Dm644 {} "${extWorkspace.getCompleteWorkspacePath()}/{}" '\';
                    
                    install -Dt "${extWorkspace.getCompleteWorkspacePath()}"/tools/objtool tools/objtool/objtool
                    
                    find -L "${extWorkspace.getCompleteWorkspacePath()}" -type l -printf 'Removing %P\n' -delete
                    """
            }
        }
    }
}
