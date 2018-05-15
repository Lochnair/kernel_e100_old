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
            steps {
                script { 
                    def extWorkspace = exwsAllocate 'diskpool1'
                    def extPath = extWorkspace.getCompleteWorkspacePath()
                    sh """
                    mkdir -p ${extPath}"
                    install -Dt "${extPath}" -m644 Makefile .config Module.symvers
                    install -Dt "${extPath}/kernel" -m644 kernel/Makefile
                    
                    mkdir "${extPath}/.tmp_versions"
                    
                    cp -t "${extPath}" -a include scripts
                    
                    install -Dt "${extPath}/arch/mips" -m644 arch/mips/Makefile
                    install -Dt "${extPath}/arch/mips/kernel" -m644 arch/mips/kernel/asm-offsets.s
                    
                    cp -t "${extPath}/arch/mips" -a arch/mips/include
                    
                    find . -name Kconfig\* -exec install -Dm644 {} "${extPath}/{}" \;
                    
                    install -Dt "${extPath}/tools/objtool" tools/objtool/objtool
                    
                    find -L "${extPath}" -type l -printf 'Removing %P\n' -delete
                    """
                }
            }
        }
    }
}
