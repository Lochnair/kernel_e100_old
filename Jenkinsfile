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

        stage('Prepare for out-of-tree builds') {
            
            steps {
                script { 
                    def extWorkspace = exwsAllocate 'pool'
                    extWorkspace.getCompleteWorkspacePath()
                }
                sh 'export'
            }
        }
    }
}
